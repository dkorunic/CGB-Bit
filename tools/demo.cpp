// Plays a scripted run of the bundled Tobu Tobu Girl Deluxe through the firmware's scaler (Game Boy
// Color colours) on the PC and writes the 128x128 RGB565 frames for make_demo_gif.py. Run by make_demo.sh.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define ENABLE_SOUND 1
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
#include "walnut_cgb.h"
#include "frontend.h"

enum : uint8_t { A = JOYPAD_A, B = JOYPAD_B, Sel = JOYPAD_SELECT, St = JOYPAD_START,
	R = JOYPAD_RIGHT, L = JOYPAD_LEFT, U = JOYPAD_UP, D = JOYPAD_DOWN, Auto = 0xFF };
struct Step { int frames; uint8_t keys; }; // keys Auto: autopilot() plays
// Tuned by watching the result; a different ROM needs its own script. Tobu Tobu Girl Deluxe: Start
// skips the logos and the intro (not recorded), then title, main menu (Plains), and the autopilot
// climbs the level until just before its first miss.
static constexpr int RecordFrom = 820;
static const Step script[] = {
	{ 240, 0 }, { 6, St }, { 234, 0 }, { 6, St }, { 234, 0 }, { 6, St }, // logos, intro
	{ 234, 0 }, { 6, St },                          // title
	{ 234, 0 }, { 6, St },                          // main menu: Plains
	{ 130, 0 },                                     // level fades in
	{ 1154, Auto },                                 // she climbs into the clouds
};

// Tobu Tobu Girl Deluxe autopilot, from the rules in its game.c: she bounces off an enemy only by
// falling onto it from above; touching one any other way kills her, and so does touching spikes or
// fireballs, or falling off the bottom. A bounce lifts her ~46 px and enemies are ~36 px apart, so
// she rises beside the next enemy up and then drops onto it. At the top of a rise she dashes up
// (A + Up) when that lands her on an enemy higher up, and when nothing below is in reach she boosts
// (B). It reads only the emulated sprites, so the run is the same every time. Sprites are told apart
// by tile (entity_sprites in game.c); each thing is two 8x16 halves.
enum Kind { None, Girl, Enemy, Hazard, Portal };
static Kind kindOf(uint8_t tile){
	if(tile < 0x18) return Girl;
	if(tile >= 0x24 && tile < 0x2C) return Hazard; // spikes
	if(tile >= 0x2C && tile < 0x4C) return Enemy; // bird, bat, ghost, alien
	if(tile >= 0x4C && tile < 0x54) return Hazard; // fireball
	if(tile >= 0x74 && tile < 0x7C) return Portal; // the level's exit
	return None; // dash counter, clock, cloud
}
static uint8_t autopilot(const gb_s& gb){
	constexpr int Side = 24; // how far beside the enemy above she rises
	constexpr int DashLow = 8, DashHigh = 44, DashNear = 40; // an enemy above to land on after a dash
	constexpr int BoostBelow = 50; // screen row under which she boosts when nothing is in reach
	struct Thing { Kind kind; int x, y; };
	Thing things[20];
	int n = 0, px = 999, py = 0, girlTile = 0;
	for(int o = 0; o < 40; o++){
		const uint8_t* e = &gb.oam[o * 4];
		const int x = e[1] - 8, y = e[0] - 16;
		if(!e[0] || e[0] >= 160 || x >= 136) continue; // hidden, or the HUD
		const Kind k = kindOf(e[2]);
		if(k == Girl){
			if(x < px){ px = x; py = y; }
			girlTile = e[2];
			continue;
		}
		if(k == None) continue;
		bool paired = false; // the other half: same kind and row, 8 px apart
		for(int i = 0; i < n; i++)
			if(things[i].kind == k && things[i].y == y && std::abs(things[i].x - x) <= 8){
				things[i].x = std::min(things[i].x, x + 8);
				paired = true;
			}
		if(!paired && n < 20) things[n++] = { k, x + 8, y };
	}
	if(px == 999) return 0;
	px += 8;
	const int frame = girlTile & 0x1C; // 0 rising, 4 falling, 8 just bounced
	const bool falling = frame == 0x04, bouncing = frame == 0x08;

	static bool wasFalling = true, dashed = true;
	static int dashLeft = 0;
	if(bouncing) dashed = false;
	const bool apex = falling && !wasFalling;
	wasFalling = falling;
	if(dashLeft > 0){
		dashLeft--;
		return A | U;
	}

	int below = 1000, belowX = px, above = 1000, aboveX = px, portalX = -1;
	bool landing = false, clearUp = true;
	for(int i = 0; i < n; i++){
		const Thing& t = things[i];
		const int dy = t.y - py, dx = std::abs(t.x - px);
		if(t.kind == Portal) portalX = t.x;
		if(t.kind == Hazard && dy < 0 && dy > -60 && dx < 16) clearUp = false;
		if(t.kind != Enemy) continue;
		if(dy >= 0 && dy < below){ below = dy; belowX = t.x; }
		if(dy < 0 && -dy < above){ above = -dy; aboveX = t.x; }
		if(dy < -DashLow && dy > -DashHigh && dx < DashNear) landing = true;
	}
	if(apex && !dashed && landing && clearUp){ // straight up: no Left/Right as it starts
		dashed = true;
		dashLeft = 13;
		return A | U;
	}

	int tx;
	if(portalX >= 0) tx = portalX;
	else if(falling || above == 1000) tx = belowX;
	else {
		tx = aboveX + (px < aboveX ? -Side : Side); // beside it until she's past it
		if(tx < 24) tx = aboveX + Side;
		if(tx > 136) tx = aboveX - Side;
	}
	for(int i = 0; i < n; i++){ // keep clear of hazards near her path, up or down
		const Thing& t = things[i];
		const int dy = t.y - py;
		if(t.kind == Hazard && std::abs(t.x - px) < 20 && (falling ? dy > -8 && dy < 48 : dy < 8 && dy > -48))
			tx = t.x < px ? t.x + 24 : t.x - 24;
	}
	uint8_t keys = tx < px - 2 ? L : tx > px + 2 ? R : 0;
	// No enemy she can still reach below (2 px sideways per frame, falling ~3 px per frame): boost.
	if(falling && py > BoostBelow && (below == 1000 || std::abs(belowX - px) > below * 2 / 3 + 6)) keys |= B;
	return keys;
}

static Scaler scaler;
static ToneApu apu; // the firmware's sound registers, so games behave the same; nothing is played
static uint8_t audio_read(uint16_t addr){ return apu.read(addr); }
static void audio_write(uint16_t addr, uint8_t val){ apu.write(addr, val); }
static std::vector<uint8_t> rom, cartRam;

static Rom romReader;
static uint8_t romRead(gb_s*, const uint_fast32_t addr){ return romReader.read8(addr); } // as in main.cpp
static uint16_t romRead16(gb_s*, const uint_fast32_t addr){ return romReader.read16(addr); }
static uint32_t romRead32(gb_s*, const uint_fast32_t addr){ return romReader.read32(addr); }
static uint8_t ramRead(gb_s*, const uint_fast32_t addr){ return addr < cartRam.size() ? cartRam[addr] : 0xFF; }
static void ramWrite(gb_s*, const uint_fast32_t addr, const uint8_t v){ if(addr < cartRam.size()) cartRam[addr] = v; }
static void onError(gb_s*, const gb_error_e e, const uint16_t addr){ fprintf(stderr, "gb error %d at %04x\n", e, addr); }
static uint16_t dmgPalette[0x40];
static void drawLine(gb_s* gb, const uint8_t* pixels, const uint_fast8_t y){ // as in main.cpp
	if(gb->cgb.cgbMode) scaler.line(pixels, y, gb->cgb.fixPalette, 0x3F);
	else scaler.line(pixels, y, dmgPalette, 0x33);
}

int main(int argc, char** argv){
	if(argc != 3){
		fprintf(stderr, "usage: %s rom.gb frames.raw\n", argv[0]);
		return 1;
	}
	FILE* f = fopen(argv[1], "rb");
	FILE* out = fopen(argv[2], "wb");
	if(!f || !out){
		perror("fopen");
		return 1;
	}
	for(int c; (c = fgetc(f)) != EOF;) rom.push_back(c);
	fclose(f);
	romReader = { rom.data(), rom.size() };

	// as in main.cpp
	static gb_s gb;
	if(!romReader.headerOk() || gb_init(&gb, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr) != GB_INIT_NO_ERROR){
		fprintf(stderr, "can't load %s\n", argv[1]);
		return 1;
	}
	romReader.clampToHeader();
	size_t saveSize = 0;
	if(gb_get_save_size_s(&gb, &saveSize) == 0) cartRam.assign(saveSize, 0);
	gb_init_lcd(&gb, drawLine);
	dmgColors(romReader, dmgPalette);

	constexpr int Top = (128 - Scaler::H) / 2;
	uint16_t screen[128][128] = {}; // black bands above and below the picture, as on the Bit
	int frame = 0;
	for(const Step& s : script)
		for(int i = 0; i < s.frames; i++, frame++){
			gb.direct.joypad = ~(s.keys == Auto ? autopilot(gb) : s.keys);
			gb_run_frame_dualfetch(&gb);
			if(frame < RecordFrom || frame % 3) continue; // 20 FPS GIF
			for(int y = 0; y < Scaler::H; y++)
				for(int x = 0; x < 128; x++) screen[Top + y][x] = scaler.out[y][x];
			fwrite(screen, 1, sizeof(screen), out);
		}
	fclose(out);
	fprintf(stderr, "%d Game Boy frames\n", frame);
	return 0;
}
