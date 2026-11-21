// Plays a scripted run of the bundled Tobu Tobu Girl Deluxe through the firmware's scaler (Game Boy
// Color colours) on the PC and writes the 128x128 RGB565 frames for make_demo_gif.py. Run by make_demo.sh.
#include <cstdint>
#include <cstdio>
#include <vector>

#define ENABLE_SOUND 1
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
#include "walnut_cgb.h"
#include "frontend.h"

enum : uint8_t { A = JOYPAD_A, B = JOYPAD_B, Sel = JOYPAD_SELECT, St = JOYPAD_START,
	R = JOYPAD_RIGHT, L = JOYPAD_LEFT, U = JOYPAD_UP, D = JOYPAD_DOWN };
struct Step { int frames; uint8_t keys; };
// Tuned by watching the result; a different ROM needs its own script. Tobu Tobu Girl Deluxe: Start
// skips the logos and the intro (not recorded), then title, main menu (Plains), bouncing between bats.
static constexpr int RecordFrom = 820;
static const Step script[] = {
	{ 240, 0 }, { 6, St }, { 234, 0 }, { 6, St }, { 234, 0 }, { 6, St }, // logos, intro
	{ 234, 0 }, { 6, St },                          // title
	{ 234, 0 }, { 6, St },                          // main menu: Plains
	{ 130, 0 },                                     // level fades in
	{ 300, 0 },                                     // she bounces between the bats
};

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
static void drawLine(gb_s* gb, const uint8_t* pixels, const uint_fast8_t y){ // as in main.cpp
	if(gb->cgb.cgbMode) scaler.line(pixels, y, gb->cgb.fixPalette, 0x3F);
	else scaler.line(pixels, y, DefaultPalette, LCD_COLOUR);
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

	constexpr int Top = (128 - Scaler::H) / 2;
	uint16_t screen[128][128] = {}; // black bands above and below the picture, as on the Bit
	int frame = 0;
	for(const Step& s : script)
		for(int i = 0; i < s.frames; i++, frame++){
			gb.direct.joypad = ~s.keys;
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
