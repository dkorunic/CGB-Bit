// Checks the front-end logic (main/frontend.h) and plays a ROM with Walnut-CGB on the PC under
// ASan/UBSan, the way the firmware drives it. Run by smoke_test.sh.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#define ENABLE_SOUND 1
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
#include "walnut_cgb.h"
#include "frontend.h"

#define CHECK(cond) do { if(!(cond)){ fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while(0)

static ToneApu apu;
static uint8_t audio_read(uint16_t addr){ return apu.read(addr); }
static void audio_write(uint16_t addr, uint8_t val){ apu.write(addr, val); }

static Scaler scaler;
static std::vector<uint8_t> romData, cartRam; // cartRam: the game's own size, as in main.cpp
static Rom rom;
static int linesThisFrame, errors, ramWrites;

// As in main.cpp, except that cart RAM accesses out of range are failures here, not 0xFF.
static uint8_t romRead(gb_s*, const uint_fast32_t addr){ return rom.read8(addr); }
static uint16_t romRead16(gb_s*, const uint_fast32_t addr){ return rom.read16(addr); }
static uint32_t romRead32(gb_s*, const uint_fast32_t addr){ return rom.read32(addr); }
static uint8_t ramRead(gb_s*, const uint_fast32_t addr){ CHECK(addr < cartRam.size()); return cartRam[addr]; }
static void ramWrite(gb_s*, const uint_fast32_t addr, const uint8_t v){ CHECK(addr < cartRam.size()); cartRam[addr] = v; ramWrites++; }
static void onError(gb_s*, const gb_error_e e, const uint16_t addr){ fprintf(stderr, "gb error %d at %04x\n", e, addr); errors++; }
static std::set<uint16_t> colorsSeen;
static uint16_t dmgPalette[0x40];
static void drawLine(gb_s* gb, const uint8_t* pixels, const uint_fast8_t y){ // as in main.cpp
	CHECK(y < LCD_HEIGHT);
	const uint16_t* palette = gb->cgb.cgbMode ? gb->cgb.fixPalette : dmgPalette;
	const uint8_t mask = gb->cgb.cgbMode ? 0x3F : 0x33;
	scaler.line(pixels, y, palette, mask);
	for(int x = 0; x < LCD_WIDTH; x++) colorsSeen.insert(palette[pixels[x] & mask]);
	linesThisFrame++;
}

static void testScaler(){
	uint8_t line[LCD_WIDTH];
	const uint16_t colors[4] = { 0xFFFF, 0xF800, 0x07E0, 0x1234 };
	for(uint16_t color : colors){ // a flat picture stays exactly the same: the weights add up to 25
		const uint16_t palette[4] = { color, color, color, color };
		memset(line, 2 | LCD_PALETTE_BG, sizeof(line)); // layer bits above the mask are ignored
		for(int y = 0; y < LCD_HEIGHT; y++) scaler.line(line, y, palette, LCD_COLOUR);
		for(auto& row : scaler.out) for(uint16_t v : row) CHECK(v == color);
	}

	// One white source pixel at (5,5) on black spans output 4..4.8 on both axes: all of it lands in
	// output (4,4), weight 16 of 25 per channel; its neighbours get nothing.
	const uint16_t blackWhite[2] = { 0x0000, 0xFFFF };
	for(int y = 0; y < LCD_HEIGHT; y++){
		memset(line, 0, sizeof(line));
		if(y == 5) line[5] = 1;
		scaler.line(line, y, blackWhite, 1);
	}
	const uint16_t share = (16 * 31 + 12) / 25 << 11 | (16 * 63 + 12) / 25 << 5 | (16 * 31 + 12) / 25;
	CHECK(scaler.out[4][4] == share && scaler.out[4][3] == 0 && scaler.out[3][4] == 0 && scaler.out[4][5] == 0);

	// Game Boy Color: pixel values index a 64-colour palette; anything above is masked, not read
	uint16_t cgb[0x40];
	for(int i = 0; i < 0x40; i++) cgb[i] = i * 0x0421;
	memset(line, 0x40 | 7, sizeof(line));
	for(int y = 0; y < LCD_HEIGHT; y++) scaler.line(line, y, cgb, 0x3F);
	CHECK(scaler.out[50][50] == cgb[7]);

	// The firmware's split (filter per line, emit per group on its other core, in panel byte order)
	// gives the same picture as line().
	srand(3);
	for(auto& c : cgb) c = rand();
	static uint16_t split[Scaler::H][Scaler::W];
	static Scaler::Rows rows;
	for(int y = 0; y < LCD_HEIGHT; y++){
		for(auto& p : line) p = rand();
		scaler.line(line, y, cgb, 0x3F);
		Scaler::filter(line, cgb, 0x3F, rows[y % 5]);
		if(const int n = Scaler::rowsDone(y)) Scaler::emit<true>(rows, n, split[y / 5 * 4]);
	}
	for(int y = 0; y < Scaler::H; y++)
		for(int x = 0; x < Scaler::W; x++) CHECK(split[y][x] == __builtin_bswap16(scaler.out[y][x]));

}

static void testApu(){
	ToneApu a;
	a.write(0xFF12, 0xF0); // NR12: volume 15, no envelope
	a.write(0xFF13, 1750 & 0xFF);
	a.write(0xFF14, 0x80 | 1750 >> 8); // trigger, no length
	CHECK(a.output().hz == 131072 / (2048 - 1750) && a.output().level == 15);
	a.tick(1000000);
	CHECK(a.output().level == 15); // no length, no envelope: plays on

	a.write(0xFF11, 63); // NR11: length 1 (1/256 s)
	a.write(0xFF14, 0xC0 | 1750 >> 8); // trigger with length on
	a.tick(10000);
	CHECK(a.output().level == 0);

	a.write(0xFF12, 0xF1); // volume 15, decreasing one step per 1/64 s
	a.write(0xFF14, 0x80 | 1750 >> 8);
	a.tick(250000); // 16 envelope steps
	CHECK(a.output().level == 0);

	a.write(0xFF17, 0x80); // channel 2 at volume 8, routed by the default NR51
	a.write(0xFF18, 0x00);
	a.write(0xFF19, 0x87);
	CHECK(a.output().hz == 131072 / (2048 - 0x700) && a.output().level == 8);
	a.write(0xFF26, 0x00); // power off
	CHECK(a.output().level == 0 && a.read(0xFF26) == 0x70);
	a.write(0xFF19, 0x87); // ignored while off
	a.write(0xFF26, 0x80);
	CHECK(a.output().level == 0);

	// Walnut-CGB's 16/32-bit reads at the end of the range ask for 0xFF40..0xFF42 too
	CHECK(a.read(0xFF3F) == 0x00 && a.read(0xFF40) == 0xFF && a.read(0xFF42) == 0xFF && a.read(0xFF0F) == 0xFF);
	a.write(0xFF40, 0x12); // ignored, nothing written past the registers
}

static void testRom(){
	std::vector<uint8_t> bytes(0x150);
	for(size_t i = 0; i < bytes.size(); i++) bytes[i] = i;
	Rom r{ bytes.data(), 64 };
	CHECK(r.read16(0) == 0x0100 && r.read16(1) == 0x0201); // aligned word load / unaligned bytes
	CHECK(r.read32(4) == 0x07060504 && r.read32(5) == 0x08070605 && r.read32(6) == 0x09080706);
	CHECK(r.read8(64) == 0xFF && r.read16(63) == 0xFF3F && r.read32(62) == 0xFFFF3F3E && r.read32(64) == 0xFFFFFFFF);

	r.size = bytes.size();
	bytes[0x148] = 3; // 256 KB
	bytes[0x149] = 5;
	CHECK(r.headerSizesOk());
	r.size = 4 << 20;
	r.clampToHeader();
	CHECK(r.size == 256 << 10);
	r.size = bytes.size();
	bytes[0x148] = 9;
	CHECK(!r.headerSizesOk());
	bytes[0x148] = 0;
	bytes[0x149] = 6;
	CHECK(!r.headerSizesOk());
}

static void testSaves(){
	CHECK(hasBattery(0x03) && hasBattery(0x1B) && hasBattery(0x10) && !hasBattery(0x02) && !hasBattery(0x1A) && !hasBattery(0x00));

	uint8_t romHeader[0x150 - 0x134] = { 'T', 'E', 'S', 'T' };
	SaveHeader slot[2] = {};
	CHECK(newestSave(slot, romHeader, 8192) == -1); // erased flash: nothing to load
	for(auto& h : slot){
		memcpy(h.magic, SaveMagic, 4);
		memcpy(h.romHeader, romHeader, sizeof(romHeader));
		h.size = 8192;
	}
	slot[0].seq = 5;
	slot[1].seq = 6;
	CHECK(newestSave(slot, romHeader, 8192) == 1);
	slot[0].seq = 0; // the sequence number wrapped: 0 comes after 0xFFFFFFFF
	slot[1].seq = 0xFFFFFFFF;
	CHECK(newestSave(slot, romHeader, 8192) == 0);
	slot[0].romHeader[0] = 'X'; // another game's save is never loaded, nor one of another size
	CHECK(newestSave(slot, romHeader, 8192) == 1);
	CHECK(newestSave(slot, romHeader, 32768) == -1);
	slot[1].magic[3] = '1'; // nor a half-written slot (no header yet) or the old one-slot format
	CHECK(newestSave(slot, romHeader, 8192) == -1);

	// Where stores go (as main.cpp's saveFlush): game A has saves in both slots, its newest in slot 0.
	uint8_t a[0x150 - 0x134] = { 'A' }, b[0x150 - 0x134] = { 'B' };
	SaveHeader s[2] = {};
	auto store = [&](int slotIndex, const uint8_t* game){
		SaveHeader h = {};
		memcpy(h.magic, SaveMagic, 4);
		h.seq = nextSeq(s);
		memcpy(h.romHeader, game, sizeof(h.romHeader));
		h.size = 8192;
		s[slotIndex] = h;
	};
	CHECK(storeSlot(s, -1) == 0 && nextSeq(s) == 1); // erased flash
	store(1, a);
	store(0, a);
	CHECK(newestSave(s, a, 8192) == 0 && storeSlot(s, 0) == 1); // a game with a save: the other slot
	// Game B's first store takes A's older slot, so A's newest survives it, and an older copy of A
	// can't come back; B's second store then takes the other slot.
	const int b1 = storeSlot(s, newestSave(s, b, 8192));
	CHECK(b1 == 1);
	store(b1, b);
	CHECK(newestSave(s, a, 8192) == 0 && newestSave(s, b, 8192) == 1);
	const int b2 = storeSlot(s, newestSave(s, b, 8192));
	CHECK(b2 == 0);
	store(b2, b);
	CHECK(newestSave(s, a, 8192) == -1 && newestSave(s, b, 8192) == 0);
	memset(s[1].magic, 0xFF, 4); // a free (erased) slot is used before any written one
	CHECK(storeSlot(s, -1) == 1);
}

// A tiny hand-assembled ROM of the given cartridge type, run for 3 frames through the same callbacks;
// returns the first byte of its cartridge RAM.
static uint8_t runCartRam(uint8_t type, uint8_t ramSizeCode, const std::vector<uint8_t>& code){
	romData.assign(0x8000, 0);
	const uint8_t entry[] = { 0x00, 0xC3, 0x50, 0x01 }; // nop; jp $0150
	memcpy(&romData[0x100], entry, sizeof(entry));
	memcpy(&romData[0x104], NintendoLogo, sizeof(NintendoLogo));
	romData[0x147] = type;
	romData[0x149] = ramSizeCode;
	uint8_t sum = 0;
	for(int i = 0x134; i <= 0x14C; i++) sum = sum - romData[i] - 1;
	romData[0x14D] = sum;
	memcpy(&romData[0x150], code.data(), code.size());
	rom = { romData.data(), romData.size() };

	static gb_s gb;
	CHECK(rom.headerOk() && gb_init(&gb, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr) == GB_INIT_NO_ERROR);
	size_t size = 0;
	CHECK(gb_get_save_size_s(&gb, &size) == 0 && size > 0);
	cartRam.assign(size, 0);
	for(int frame = 0; frame < 3; frame++) gb_run_frame_dualfetch(&gb);
	return cartRam[0];
}

// The Walnut-CGB fixes carried in patches/ (upstream PRs #7 and #8).
static void testCartRam(){
	// MBC2+BATTERY (its RAM is built in, header RAM size 0): enable RAM, write 0x05 to 0xA000. The
	// 4-bit RAM reads back with the upper nibble high. Unpatched, the write never arrives.
	CHECK(runCartRam(0x06, 0, { 0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x05, 0xEA, 0x00, 0xA0, 0x18, 0xFE }) == 0xF5);
	// MBC5+RUMBLE+RAM+BATTERY with 8 KiB: select RAM bank 0 with the motor bit (0x08), write 0x42 to
	// 0xA000. It lands at offset 0; unpatched it never arrives, or at 0x10000 (ramWrite's CHECK).
	CHECK(runCartRam(0x1E, 2, { 0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x08, 0xEA, 0x00, 0x40, 0x3E, 0x42, 0xEA, 0x00, 0xA0, 0x18, 0xFE }) == 0x42);
	ramWrites = 0;
	romData.clear(); // main() loads the game into it next
}

// The palettes a Game Boy Color's boot ROM gives original Game Boy games (cgb_palettes.h).
static void testDmgColors(){
	std::vector<uint8_t> h(0x150, 0);
	const Rom r{ h.data(), h.size() };
	uint16_t pal[0x40];
	auto title = [&](const char* t, uint8_t licensee){
		memset(&h[0x134], 0, 16);
		memcpy(&h[0x134], t, strlen(t));
		h[0x14B] = licensee;
		dmgColors(r, pal);
	};
	auto layer = [&](int l, std::initializer_list<uint16_t> bgr555){ // OBJ0, OBJ1, BG
		int shade = 0;
		for(uint16_t c : bgr555) if(pal[l << 4 | shade++] != bgr555_to_rgb565_accurate(c)) return false;
		return true;
	};
	title("TETRIS", 0x00); // not published by Nintendo: the default
	CHECK(layer(0, { 0x7FFF, 0x421F, 0x1CF2, 0x0000 }) && layer(1, { 0x7FFF, 0x421F, 0x1CF2, 0x0000 }) && layer(2, { 0x7FFF, 0x1BEF, 0x6180, 0x0000 }));
	title("TETRIS", 0x01); // title checksum 0xDB: palette 24 for all three
	CHECK(layer(0, { 0x7FFF, 0x03FF, 0x001F, 0x0000 }) && layer(1, { 0x7FFF, 0x03FF, 0x001F, 0x0000 }) && layer(2, { 0x7FFF, 0x03FF, 0x001F, 0x0000 }));
	h[0x144] = '0'; // the new licensee code "01" is Nintendo too
	h[0x145] = '1';
	title("TETRIS", 0x33);
	CHECK(layer(2, { 0x7FFF, 0x03FF, 0x001F, 0x0000 }));
	title("SUPER MARIOLAND", 0x01); // checksum 0x46 is shared, the 4th letter picks this one
	CHECK(layer(0, { 0x0000, 0x7FFF, 0x421F, 0x1CF2 }) && layer(2, { 0x7ED6, 0x4BFF, 0x2175, 0x0000 }));
	title("SUPXR MARIOLAND\xED", 0x01); // the same checksum with a 4th letter of none of them: the default
	CHECK(layer(2, { 0x7FFF, 0x1BEF, 0x6180, 0x0000 }));
	CHECK(pal[0x30] == 0 && pal[0x3F] == 0);
}

static void testMenu(){
	MenuButton m;
	uint32_t t = 0;
	CHECK(m.update(true, t) == 0);
	CHECK(m.update(false, t += 100) == JOYPAD_START); // short press: Start, for a few frames
	for(int i = 1; i < MenuButton::StartFrames; i++) CHECK(m.update(false, t += 17) == JOYPAD_START);
	CHECK(m.update(false, t += 17) == 0);
	m.update(true, t);
	CHECK(m.update(true, t += 400) == 0);
	CHECK(m.update(true, t += 200) == JOYPAD_SELECT); // held: Select, and no Start on release
	CHECK(m.update(false, t += 17) == 0);
}

int main(int argc, char** argv){
	testScaler();
	testApu();
	testRom();
	testSaves();
	testCartRam();
	testDmgColors();
	testMenu();
	printf("front-end checks passed\n");

	if(argc < 2){
		fprintf(stderr, "usage: %s rom.gb\n", argv[0]);
		return 1;
	}
	FILE* f = fopen(argv[1], "rb");
	CHECK(f);
	int c;
	while((c = fgetc(f)) != EOF) romData.push_back(c);
	fclose(f);
	rom = { romData.data(), romData.size() };
	CHECK(rom.headerOk());
	romData[0x104] ^= 1; // no Nintendo logo: refused, as the boot ROM of a real Game Boy would
	CHECK(!rom.headerOk());
	romData[0x104] ^= 1;

	static gb_s gb;
	CHECK(gb_init(&gb, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr) == GB_INIT_NO_ERROR);
	gb_init_lcd(&gb, drawLine);
	dmgColors(rom, dmgPalette);
	rom.clampToHeader();
	size_t saveSize = 0;
	CHECK(gb_get_save_size_s(&gb, &saveSize) == 0 && saveSize <= 0x20000);
	cartRam.assign(saveSize, 0);

	// One emulated minute: Start through the title and menus, then move and press A and B at random.
	srand(1);
	int drawnFrames = 0, tonedFrames = 0;
	MenuButton menu;
	for(int frame = 0; frame < 60 * 60; frame++){
		const bool menuDown = frame % 120 < 4 && frame < 900; // short presses every 2 s at first
		uint8_t pressed = menu.update(menuDown, frame * 1000 / 60);
		if(frame >= 900 && frame % 10 == 0) pressed |= (rand() & (JOYPAD_A | JOYPAD_B | JOYPAD_LEFT | JOYPAD_RIGHT));
		gb.direct.joypad = ~pressed;

		linesThisFrame = 0;
		gb_run_frame_dualfetch(&gb);
		// whole frames only, or none while the LCD is off; emulation starts one line into frame 0
		CHECK(linesThisFrame == 0 || linesThisFrame == LCD_HEIGHT || (frame == 0 && linesThisFrame == LCD_HEIGHT - 1));
		drawnFrames += linesThisFrame == LCD_HEIGHT;
		apu.tick(16743);
		tonedFrames += apu.output().level > 0;
	}
	char title[17];
	printf("%s (%s mode): %d of 3600 frames drawn, %zu colours, %d with a tone, %d cart RAM writes (save size %zu)\n",
		gb_get_rom_name(&gb, title), gb.cgb.cgbMode ? "Color" : "original", drawnFrames, colorsSeen.size(), tonedFrames, ramWrites, saveSize);
	CHECK(errors == 0);
	CHECK(drawnFrames > 3000);
	CHECK(tonedFrames > 0);
	if(gb.cgb.cgbMode) CHECK(colorsSeen.size() > 4); // real colours, not the 4 original shades

	// Suspend and resume: a state taken now and loaded into a freshly started emulator plays on
	// exactly as this one does (picture, sound and cartridge RAM, frame by frame).
	std::vector<uint8_t> state(stateSize(cartRam.size()));
	CHECK(stateParts(gb, apu, cartRam.data(), cartRam.size(), [&](size_t at, void* p, size_t n){ memcpy(&state[at], p, n); return true; }));
	static Scaler screen;
	screen = scaler; // the screen isn't in a state, the firmware just draws the next frame
	auto playOn = [&](gb_s& g){
		std::vector<uint64_t> hashes;
		srand(2);
		for(int frame = 0; frame < 600; frame++){
			g.direct.joypad = ~(rand() & (JOYPAD_A | JOYPAD_B | JOYPAD_LEFT | JOYPAD_RIGHT));
			gb_run_frame_dualfetch(&g);
			apu.tick(16743);
			uint64_t h = 1469598103934665603u ^ apu.output().hz ^ uint64_t(apu.output().level) << 32;
			for(const auto* p = (const uint8_t*) scaler.out; p < (const uint8_t*) scaler.out + sizeof(scaler.out); p++) h = (h ^ *p) * 1099511628211u;
			for(uint8_t b : cartRam) h = (h ^ b) * 1099511628211u;
			hashes.push_back(h);
		}
		return hashes;
	};
	const auto expected = playOn(gb);
	static gb_s resumed;
	CHECK(gb_init(&resumed, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr) == GB_INIT_NO_ERROR);
	gb_init_lcd(&resumed, drawLine);
	int marker;
	resumed.direct.priv = &marker; // this boot's: kept, not the state's (null)
	apu = ToneApu();
	std::fill(cartRam.begin(), cartRam.end(), 0xAA);
	scaler = screen;
	CHECK(loadState(resumed, apu, cartRam.data(), cartRam.size(), [&](size_t at, void* p, size_t n){ memcpy(p, &state[at], n); return true; }));
	CHECK(resumed.direct.priv == &marker && resumed.display.lcd_draw_line == drawLine && resumed.gb_cart_ram_write == ramWrite);
	CHECK(playOn(resumed) == expected);
	printf("resumed state (%zu bytes) plays on identically\n", state.size());
	printf("smoke test passed\n");
}
