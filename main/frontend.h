// Game Boy front-end logic with no ESP-IDF dependencies, shared by the firmware (main.cpp) and the
// PC tools (tools/smoke_test.cpp, tools/demo.cpp). Include walnut_cgb.h first.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

// The logo every Game Boy ROM header carries at 0x104, which a real Game Boy's boot ROM checks.
constexpr uint8_t NintendoLogo[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
	0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
	0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E };

// The game's ROM (the firmware: the memory-mapped rom partition). Reads past its end give 0xFF.
// Walnut-CGB's dual-fetch core also reads 2 and 4 bytes at once; memory-mapped flash only allows
// aligned word loads, so those are used for aligned addresses and bytes otherwise. `data` must be
// 4-byte aligned (a flash mapping is page-aligned).
struct Rom {
	const uint8_t* data = nullptr;
	size_t size = 0;

	uint8_t read8(uint_fast32_t addr) const { return addr < size ? data[addr] : 0xFF; }
	uint16_t read16(uint_fast32_t addr) const {
		if(addr & 1 || addr + 2 > size) return read8(addr) | read8(addr + 1) << 8;
		uint16_t v;
		memcpy(&v, __builtin_assume_aligned(data + addr, 2), 2);
		return v;
	}
	uint32_t read32(uint_fast32_t addr) const {
		if(addr & 3 || addr + 4 > size) return read16(addr) | (uint32_t) read16(addr + 2) << 16;
		uint32_t v;
		memcpy(&v, __builtin_assume_aligned(data + addr, 4), 4);
		return v;
	}

	// Walnut-CGB's gb_init indexes its ROM and RAM size tables with header bytes 0x148 and 0x149
	// unchecked; web/gb.js rejects the same files before flashing them.
	bool headerSizesOk() const { return read8(0x148) <= 8 && read8(0x149) <= 5; }
	// The Nintendo logo (gb_init doesn't check it); web/gb.js checks it too.
	bool logoOk() const {
		for(int i = 0; i < 48; i++) if(read8(0x104 + i) != NintendoLogo[i]) return false;
		return true;
	}
	bool headerOk() const { return logoOk() && headerSizesOk(); }
	// The header's ROM size, if smaller than what holds the ROM (a partition is larger than its game).
	void clampToHeader(){ if(headerSizesOk()) size = std::min<size_t>(size, size_t(0x8000) << read8(0x148)); }
};

// Cartridge types (header byte 0x147) whose RAM is battery-backed. Only these keep their RAM when the
// Game Boy is switched off, so only these are saved; the others use it as scratch memory.
inline bool hasBattery(uint8_t cartType){
	switch(cartType){
		case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E: return true;
		default: return false;
	}
}

// Battery RAM is stored in two slots, alternately, so a power cut while storing keeps the previous
// save. Each slot is [header sector][data]; the header names the game (its ROM header 0x134..0x14F),
// is written last, and carries a sequence number: the newer valid slot for this game is loaded.
struct SaveHeader {
	char magic[4];
	uint32_t seq;
	uint8_t romHeader[0x150 - 0x134];
	uint32_t size;
};
constexpr char SaveMagic[4] = { 'G', 'B', 'S', '2' };

// Where the next store goes and its sequence number, given the slots' headers and the slot holding
// this game's current save (-1: none yet). With a save, the other slot. Without one, the free slot
// or the older one, so the previous game's newest save outlives this game's first store and an
// older copy of it can never come back; seq above both slots keeps "older" well defined.
inline int storeSlot(const SaveHeader slot[2], int current){
	if(current >= 0) return 1 - current;
	for(int i = 0; i < 2; i++) if(memcmp(slot[i].magic, SaveMagic, 4)) return i;
	return int32_t(slot[1].seq - slot[0].seq) < 0 ? 1 : 0;
}
inline uint32_t nextSeq(const SaveHeader slot[2]){
	uint32_t seq = 0;
	bool any = false;
	for(int i = 0; i < 2; i++)
		if(!memcmp(slot[i].magic, SaveMagic, 4) && (!any || int32_t(slot[i].seq - seq) > 0)){
			seq = slot[i].seq;
			any = true;
		}
	return seq + 1;
}

// Slot (0, 1) holding this game's newest save of `size` bytes, or -1.
inline int newestSave(const SaveHeader slot[2], const uint8_t* romHeader, uint32_t size){
	int best = -1;
	for(int i = 0; i < 2; i++){
		const SaveHeader& h = slot[i];
		if(memcmp(h.magic, SaveMagic, 4) || memcmp(h.romHeader, romHeader, sizeof(h.romHeader)) || h.size != size) continue;
		if(best < 0 || int32_t(h.seq - slot[best].seq) > 0) best = i; // wrap-safe "newer"
	}
	return best;
}

// 160x144 Game Boy picture into 128x115 RGB565: an exact 5:4 box filter on both axes, per colour
// channel. Output pixel j covers source pixels 5k+m and 5k+m+1 (k = j/4, m = j%4) with weights 4-m
// and 1+m; the four weights of a pixel add up to 25, so a flat colour stays exactly the same.
struct Scaler {
	static constexpr int W = 128, H = 115;
	uint16_t out[H][W] = {};
	uint16_t ring[5][LCD_WIDTH] = {};

	// Called for lines 0..143 of every frame. palette[pixel & mask] is the RGB565 colour of a pixel:
	// 4 shades with mask 3 for original Game Boy games, Walnut-CGB's 64-entry fixPalette with mask
	// 0x3F for Game Boy Color ones.
	void line(const uint8_t* pixels, unsigned y, const uint16_t* palette, uint8_t mask){
		for(int x = 0; x < LCD_WIDTH; x++) ring[y % 5][x] = palette[pixels[x] & mask];
		if(y % 5 == 4) emit(y / 5, 4);
		else if(y == LCD_HEIGHT - 1) emit(y / 5, 3); // lines 140..143: the 4th row would need line 144
	}

private:
	void emit(unsigned group, int rows){
		for(int r = 0; r < rows; r++){
			const uint16_t* a = ring[r];
			const uint16_t* b = ring[r + 1];
			const int wa = 4 - r, wb = 1 + r;
			for(int j = 0; j < W; j++){
				const int m = j % 4, s = 5 * (j / 4) + m, w0 = 4 - m, w1 = 1 + m;
				const uint16_t px[4] = { a[s], a[s + 1], b[s], b[s + 1] };
				const int w[4] = { wa * w0, wa * w1, wb * w0, wb * w1 };
				int red = 0, green = 0, blue = 0;
				for(int k = 0; k < 4; k++){
					red += w[k] * (px[k] >> 11);
					green += w[k] * (px[k] >> 5 & 63);
					blue += w[k] * (px[k] & 31);
				}
				out[group * 4 + r][j] = (red + 12) / 25 << 11 | (green + 12) / 25 << 5 | (blue + 12) / 25;
			}
		}
	}
};

constexpr uint16_t rgb565(uint32_t rgb888){
	return (rgb888 >> 19 & 0x1F) << 11 | (rgb888 >> 10 & 0x3F) << 5 | (rgb888 >> 3 & 0x1F);
}

// The 4 shades of original Game Boy games on the Bit's screen, lightest first: a soft green in the
// spirit of the original screen, with enough contrast for the small panel. (Game Boy Color games
// bring their own colours.)
constexpr uint16_t DefaultPalette[4] = { rgb565(0xE0F8D0), rgb565(0x88C070), rgb565(0x346856), rgb565(0x081820) };

// The Game Boy's two square-wave channels, tracked from the game's register writes, reduced to the
// one tone a buzzer can play: the loudest audible channel. Sweep (NR10), duty, the wave and noise
// channels are not modelled. Register reads behave like Walnut-CGB (Peanut-GB) without sound support.
struct ToneApu {
	struct Tone { uint32_t hz; uint8_t level; }; // level 0..15

	ToneApu(){
		regs[0x14] = 0x77; // NR50, NR51, NR52 as the boot ROM leaves them: full volume, sound on
		regs[0x15] = 0xF3;
		regs[0x16] = 0x80;
	}

	// Walnut-CGB's 16- and 32-bit reads call this for addr + 1..3 too, which can pass 0xFF3F.
	uint8_t read(uint16_t addr) const {
		if(addr < 0xFF10 || addr >= 0xFF40) return 0xFF;
		static constexpr uint8_t mask[0x30] = {
			0x80, 0x3f, 0x00, 0xff, 0xbf, 0xff, 0x3f, 0x00, 0xff, 0xbf, 0x7f, 0xff, 0x9f, 0xff, 0xbf, 0xff,
			0xff, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x70, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		return regs[addr - 0xFF10] | mask[addr - 0xFF10];
	}

	void write(uint16_t addr, uint8_t v){
		if(addr < 0xFF10 || addr >= 0xFF40) return;
		const int i = addr - 0xFF10;
		if(addr == 0xFF26){ // NR52: power off clears NR10..NR51 and silences everything
			if(!(v & 0x80)){
				std::memset(regs, 0, 0x16);
				ch[0] = ch[1] = Channel{};
			}
			regs[i] = v & 0x80;
			return;
		}
		if(!(regs[0x16] & 0x80) && addr < 0xFF26) return; // ignored while powered off (wave RAM isn't)
		regs[i] = v;
		for(int c = 0; c < 2; c++){
			const int base = c ? 6 : 1; // NR11 / NR21; then NRx2 volume, NRx3 freq low, NRx4 control
			Channel& h = ch[c];
			if(i == base) h.length = 64 - (v & 63);
			else if(i == base + 1 && !(v & 0xF8)) h.on = false; // DAC off
			else if(i == base + 3){
				h.lengthOn = v & 0x40;
				if(v & 0x80){ // trigger
					const uint8_t env = regs[base + 1];
					h.on = env & 0xF8;
					h.vol = env >> 4;
					h.envUp = env & 8;
					h.envPeriod = h.envTimer = env & 7;
					if(!h.length) h.length = 64;
				}
			}
		}
	}

	// Advance the 512 Hz frame sequencer: length counters at 256 Hz, envelopes at 64 Hz.
	void tick(uint32_t us){
		seqUs += us;
		for(; seqUs >= 1953; seqUs -= 1953){
			step = (step + 1) & 7;
			for(Channel& h : ch){
				if(!h.on) continue;
				if(!(step & 1) && h.lengthOn && --h.length == 0) h.on = false;
				if(step == 7 && h.envPeriod && --h.envTimer == 0){
					h.envTimer = h.envPeriod;
					if(h.envUp && h.vol < 15) h.vol++;
					else if(!h.envUp && h.vol > 0) h.vol--;
				}
			}
		}
	}

	Tone output() const {
		if(!(regs[0x16] & 0x80)) return { 0, 0 };
		const uint8_t nr50 = regs[0x14], nr51 = regs[0x15];
		const int master = std::max(nr50 & 7, nr50 >> 4 & 7) + 1; // 1..8
		Tone best{ 0, 0 };
		for(int c = 0; c < 2; c++){
			const Channel& h = ch[c];
			if(!h.on || !h.vol || !(nr51 >> c & 0x11)) continue; // off, silent, or routed to neither side
			const uint8_t level = h.vol * master / 8;
			if(level > best.level) best = { hz(c), level };
		}
		return best;
	}

	uint32_t hz(int c) const {
		const int base = c ? 6 : 1;
		return 131072 / (2048 - (regs[base + 2] | (regs[base + 3] & 7) << 8));
	}

private:
	struct Channel { bool on, envUp, lengthOn; uint8_t vol, envPeriod, envTimer, length; };
	uint8_t regs[0x30] = {};
	Channel ch[2] = {};
	uint32_t seqUs = 0;
	uint8_t step = 0;
};

// The Bit's Menu button as Start and Select: a short press is a Start press (sent on release, for a
// few frames so the game sees it); holding it for LongMs or more is Select, for as long as it's held.
struct MenuButton {
	static constexpr uint32_t LongMs = 500;
	static constexpr int StartFrames = 6;

	uint8_t update(bool pressed, uint32_t nowMs){ // returns JOYPAD_START / JOYPAD_SELECT bits to press
		if(pressed && !down){
			down = true;
			downAt = nowMs;
		}else if(!pressed && down){
			down = false;
			if(nowMs - downAt < LongMs) startLeft = StartFrames;
		}
		uint8_t bits = 0;
		if(down && nowMs - downAt >= LongMs) bits |= JOYPAD_SELECT;
		if(startLeft > 0){
			startLeft--;
			bits |= JOYPAD_START;
		}
		return bits;
	}

private:
	bool down = false;
	uint32_t downAt = 0;
	int startLeft = 0;
};
