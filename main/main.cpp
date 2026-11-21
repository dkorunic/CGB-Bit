/*
  Game Boy and Game Boy Color (Walnut-CGB) front end for CircuitMess Bit (ESP32-S3, ST7735S 128x128,
  7 buttons, buzzer). Hardware setup mirrors Bit-Firmware (Pins.cpp, Display.cpp) via Anarch-Bit.
*/

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <esp_efuse.h>
#include <esp_sleep.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <LovyanGFX.hpp>

#define ENABLE_SOUND 1
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
#include "walnut_cgb.h"
#include "frontend.h"

// Pin numbers from Bit-Firmware Pins.cpp: rev 0-2 = Bit / Bit 2, rev 3-4 = Bit 3.
struct BitPins { int up, down, left, right, a, b, menu, bl, buzz, sck, mosi, dc, rst, batt, vref; };
static constexpr BitPins PinsRev1 = { 5, 4, 3, 6, 7, 8, 9, 12, 11, 13, 14, 15, 16, 10, -1 };
static constexpr BitPins PinsRev3 = { 11, 13, 12, 9, 14, 15, 21, 8, 16, 4, 5, 6, 7, 3, 2 };
static BitPins pins;

static constexpr int Screen = 128;
static constexpr int Top = (Screen - Scaler::H) / 2; // the 128x115 picture, centred: 6 black rows above
static constexpr int64_t FrameUs = 16743; // 70224 cycles at 4.194304 MHz: 59.73 frames per second

// Two frames: the emulator draws into one while DMA sends the other to the panel.
DMA_ATTR static uint16_t frames[2][Screen * Screen];
static uint16_t* frame = frames[0];

static lgfx::Bus_SPI bus;
static lgfx::Panel_ST7735S panel;
static LGFX_Device lcd;

static constexpr uint32_t SleepMs = 2 * 60 * 1000; // Bit-Firmware default auto-sleep: 2 min without input
static constexpr uint32_t DimMs = SleepMs - 10 * 1000; // dim the screen 10 s before, as a warning

static constexpr uint8_t Brightness = 100; // 0-100, Bit-Firmware default screenBrightness
static uint32_t blDuty; // current backlight duty, 0 = off
static uint32_t blFull; // duty for Brightness

static bool lowBattery; // stock BatteryElement blinks its empty icon below 15 %

static adc_oneshot_unit_handle_t adcUnit;
static adc_channel_t battChan;
static adc_cali_handle_t battCali; // rev 3+ only
static float battOffset; // mV: rev 3+ from the vref calibration, rev 1 from efuse
static float battEma = -1;
static bool battMonitor = true; // off when the chip has no ADC calibration to compute volts from

static gb_s gb;
static ToneApu apu;
static Scaler scaler;
static Rom rom;
static uint16_t dmgPalette[0x40]; // original Game Boy games: the colours a Game Boy Color gives them

// Cartridge RAM, and for battery-backed cartridges its two save slots in the saves partition (see
// SaveHeader in frontend.h): each slot is a header sector then up to 128 KB of data.
static constexpr size_t SaveData = 0x1000, SaveSlot = SaveData + 0x20000;
static const esp_partition_t* savePart; // null: nothing to keep (no battery) or no room
static int saveSlot = -1; // slot holding this game's current save
static SaveHeader saveHeaders[2]; // what the two slots hold on flash
static uint8_t* cartRam;
static size_t cartRamSize;
static bool ramDirty;
static uint32_t ramWriteMs, ramDirtyMs, flushMs; // last write, first unsaved write, last store

// The whole emulator at shutdown, resumed at the next boot (StateHeader in frontend.h).
static constexpr size_t StatePartSize = 0x30000; // partitions.csv
static_assert(stateSize(0x20000) <= StatePartSize, "the state partition must hold the largest cartridge RAM");
static const esp_partition_t* statePart; // null: no resume
static bool running; // a game is running: shutdown() suspends it

static uint32_t millis(){
	return esp_timer_get_time() / 1000;
}

static uint8_t readRevision(){
	static const esp_efuse_desc_t revField = { EFUSE_BLK3, 32, 8 }; // EfuseMeta.h
	static const esp_efuse_desc_t* revBlob[] = { &revField, nullptr };
	uint8_t rev = 0;
	esp_efuse_read_field_blob(revBlob, &rev, 8);
	return rev;
}

static void initDisplay(uint8_t rev){
	auto b = bus.config();
	b.spi_host = SPI2_HOST;
	b.spi_mode = 0;
	b.freq_write = 40000000;
	b.pin_sclk = pins.sck;
	b.pin_mosi = pins.mosi;
	b.pin_miso = -1;
	b.pin_dc = pins.dc;
	b.dma_channel = SPI_DMA_CH_AUTO;
	bus.config(b);
	panel.setBus(&bus);

	auto p = panel.config();
	p.pin_cs = -1;
	p.pin_rst = pins.rst;
	p.pin_busy = -1;
	p.panel_width = 128;
	p.panel_height = 128;
	p.readable = false;
	p.invert = false;
	p.rgb_order = false;
	p.dlen_16bit = false;
	p.bus_shared = false;
	if(rev == 4){
		p.memory_width = p.memory_height = 128;
		p.offset_x = p.offset_y = 0;
		p.offset_rotation = 0;
	}else{
		p.memory_width = p.memory_height = 132;
		p.offset_x = 2;
		p.offset_y = 1;
		p.offset_rotation = rev == 3 ? 0 : 2;
	}
	panel.config(p);
	lcd.setPanel(&panel);
	lcd.init();
	lcd.fillScreen(0);
}

static void initBacklight(){
	ledc_timer_config_t t = {};
	t.speed_mode = LEDC_LOW_SPEED_MODE;
	t.duty_resolution = LEDC_TIMER_10_BIT;
	t.timer_num = LEDC_TIMER_1; // own timer: the buzzer keeps changing LEDC_TIMER_0's frequency
	t.freq_hz = 5000;
	t.clk_cfg = LEDC_USE_XTAL_CLK; // S3: one clock source for all LEDC timers, see initBuzzer
	ESP_ERROR_CHECK(ledc_timer_config(&t));

	ledc_channel_config_t c = {};
	c.gpio_num = pins.bl;
	c.speed_mode = LEDC_LOW_SPEED_MODE;
	c.channel = LEDC_CHANNEL_1;
	c.timer_sel = LEDC_TIMER_1;
	c.duty = 0;
	c.flags.output_invert = 1; // as Bit-Firmware's PWM(LedBl, invert): duty 0 = backlight off
	ESP_ERROR_CHECK(ledc_channel_config(&c));

	const float level = (10 + Brightness * 0.9f) / 100; // BacklightBrightness::mapDuty: min 10%, squared
	blFull = std::lround(level * level * 100) * 1024 / 100;
}

static void backlightSet(uint32_t duty){
	blDuty = duty;
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

// BacklightBrightness::fadeIn/fadeOut: 100 steps of 2 ms, here from the current duty to any other.
static void backlightFade(uint32_t to){
	const int from = blDuty;
	if(from == (int) to) return;
	for(int i = 1; i <= 100; i++){
		backlightSet(from + ((int) to - from) * i / 100);
		vTaskDelay(pdMS_TO_TICKS(2));
	}
}

static void buzzerDuty(uint32_t duty){
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void initBuzzer(){
	ledc_timer_config_t t = {};
	t.speed_mode = LEDC_LOW_SPEED_MODE;
	t.duty_resolution = LEDC_TIMER_10_BIT;
	t.timer_num = LEDC_TIMER_0;
	t.freq_hz = 1000;
	t.clk_cfg = LEDC_USE_XTAL_CLK; // 40 MHz crystal: 10-bit duty reaches 38 Hz-39 kHz
	ESP_ERROR_CHECK(ledc_timer_config(&t));

	ledc_channel_config_t c = {};
	c.gpio_num = pins.buzz;
	c.speed_mode = LEDC_LOW_SPEED_MODE;
	c.channel = LEDC_CHANNEL_0;
	c.timer_sel = LEDC_TIMER_0;
	c.duty = 0;
	ESP_ERROR_CHECK(ledc_channel_config(&c));
}

// The loudest square channel on the buzzer, louder as a wider duty (at most ~50%).
static void buzzerTone(ToneApu::Tone t){
	static uint32_t hz;
	if(!t.level || t.hz < 40 || t.hz > 15000){ // silent, or outside what the buzzer and LEDC do well
		buzzerDuty(0);
		return;
	}
	if(t.hz != hz && ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, t.hz) == ESP_OK) hz = t.hz;
	buzzerDuty(t.level * 34);
}

// Store into the slot storeSlot picks, never the one with this game's current save: a power cut
// midway leaves that one intact (the header goes in last, so the half-written slot never loads).
// A failed store stays dirty and is tried again.
static void saveFlush(){
	if(!ramDirty || !savePart) return;
	buzzerDuty(0); // erasing pauses the game: don't drone the last note meanwhile
	const int slot = storeSlot(saveHeaders, saveSlot);
	SaveHeader h;
	memcpy(h.magic, SaveMagic, sizeof(h.magic));
	h.seq = nextSeq(saveHeaders);
	memcpy(h.romHeader, rom.data + 0x134, sizeof(h.romHeader));
	h.size = cartRamSize;
	const size_t base = slot * SaveSlot;
	const bool ok = esp_partition_erase_range(savePart, base, (SaveData + cartRamSize + 4095) & ~4095u) == ESP_OK
		&& esp_partition_write(savePart, base + SaveData, cartRam, cartRamSize) == ESP_OK
		&& esp_partition_write(savePart, base, &h, sizeof(h)) == ESP_OK;
	if(ok){
		saveHeaders[slot] = h;
		saveSlot = slot;
		ramDirty = false;
	}else{
		memset(&saveHeaders[slot], 0, sizeof(SaveHeader)); // erased or half written: no save there now
	}
	printf("Save %s (slot %d)\n", ok ? "stored" : "store failed, will retry", slot);
}

// Suspend: called by shutdown() after saveFlush(), so ramSaved tells whether the battery save holds
// the same cartridge RAM as the state.
static void stateStore(){
	if(!statePart) return;
	buzzerDuty(0); // storing takes a moment
	StateHeader h = {};
	memcpy(h.magic, StateMagic, sizeof(h.magic));
	memcpy(h.build, esp_app_get_description()->app_elf_sha256, sizeof(h.build));
	memcpy(h.romHeader, rom.data + 0x134, sizeof(h.romHeader));
	h.ramSize = cartRamSize;
	h.ramSaved = savePart && saveSlot >= 0 && !ramDirty;
	h.saveSeq = h.ramSaved ? saveHeaders[saveSlot].seq : 0;
	const bool ok = esp_partition_erase_range(statePart, 0, (stateSize(cartRamSize) + 4095) & ~4095u) == ESP_OK
		&& stateParts(gb, apu, cartRam, cartRamSize, [](size_t at, void* p, size_t n){ return esp_partition_write(statePart, at, p, n) == ESP_OK; })
		&& esp_partition_write(statePart, 0, &h, sizeof(h)) == ESP_OK; // last: a cut-short store never loads
	printf("State %s\n", ok ? "stored" : "store failed");
}

// Resume the state stored at the last shutdown, once: its header is erased before loading, so after
// a power cut later (the power switch skips shutdown()) the game starts afresh, not back here.
static void stateResume(){
	statePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x42, "state");
	if(!statePart || stateSize(cartRamSize) > statePart->size){
		statePart = nullptr;
		printf("No state partition: games won't resume after sleep\n");
		return;
	}
	StateHeader h;
	if(esp_partition_read(statePart, 0, &h, sizeof(h)) != ESP_OK
		|| !stateMatches(h, esp_app_get_description()->app_elf_sha256, rom.data + 0x134, cartRamSize)
		|| esp_partition_erase_range(statePart, 0, StateData) != ESP_OK) return;
	if(!loadState(gb, apu, cartRam, cartRamSize, [](size_t at, void* p, size_t n){ return esp_partition_read(statePart, at, p, n) == ESP_OK; })){
		printf("State load failed, restarting\n"); // the emulator is half overwritten, the state erased
		esp_restart();
	}
	// The resumed cartridge RAM replaces the battery save just loaded: store it unless they're the same.
	if(savePart && !(h.ramSaved && saveSlot >= 0 && saveHeaders[saveSlot].seq == h.saveSeq)){
		ramDirty = true;
		ramWriteMs = ramDirtyMs = millis();
	}
	printf("Resumed\n");
}

static void saveLoad(){
	savePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x41, "saves");
	if(!savePart || 2 * SaveSlot > savePart->size){
		savePart = nullptr;
		printf("No saves partition: saves won't be kept\n");
		return;
	}
	for(int i = 0; i < 2; i++)
		if(esp_partition_read(savePart, i * SaveSlot, &saveHeaders[i], sizeof(SaveHeader)) != ESP_OK)
			memset(&saveHeaders[i], 0, sizeof(SaveHeader));
	const int slot = newestSave(saveHeaders, rom.data + 0x134, cartRamSize);
	if(slot >= 0 && esp_partition_read(savePart, slot * SaveSlot + SaveData, cartRam, cartRamSize) == ESP_OK){
		saveSlot = slot;
		printf("Save loaded (%u bytes, slot %d)\n", (unsigned) cartRamSize, slot);
	}
}

// Same as Bit-Firmware shutdown(): park the pins so nothing leaks current, then sleep until power cycle.
[[noreturn]] static void shutdown(){
	saveFlush();
	if(running) stateStore();
	backlightFade(0);
	const struct { int pin, level; gpio_pull_mode_t pull; } park[] = {
		{ pins.bl, 1, GPIO_PULLUP_ONLY }, { pins.buzz, 0, GPIO_PULLDOWN_ONLY }, { pins.vref, 0, GPIO_PULLDOWN_ONLY } };
	for(const auto& p : park){
		if(p.pin < 0) continue;
		gpio_set_direction((gpio_num_t) p.pin, GPIO_MODE_OUTPUT);
		gpio_set_level((gpio_num_t) p.pin, p.level);
		gpio_set_pull_mode((gpio_num_t) p.pin, p.pull);
		gpio_hold_en((gpio_num_t) p.pin);
	}
	esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
	gpio_deep_sleep_hold_en();
	esp_deep_sleep_start();
}

static bool readBatt(float& mV){
	int v = 0;
	const esp_err_t err = battCali ? adc_oneshot_get_calibrated_result(adcUnit, battCali, battChan, &v) : adc_oneshot_read(adcUnit, battChan, &v);
	if(err == ESP_OK) mV = v;
	return err == ESP_OK;
}

static void sampleBatt(){
	if(!battMonitor) return;
	float v;
	if(readBatt(v)) battEma = battEma < 0 ? v : battEma * 0.95f + v * 0.05f;
}

// Battery percentage exactly as Bit-Firmware BatteryRev3 / BatteryRev1 (ADCReader) compute it.
static float battPercent(){
	if(!battMonitor || battEma < 0) return 100; // unknown (no calibration, or no reading yet): never shut down for it
	float mV, lo, hi;
	if(battCali){
		mV = (int) (-1.1316f * (377.595f - (battEma * 4 + battOffset)));
		lo = 3600; hi = 4150;
	}else{
		mV = battEma + battOffset;
		lo = 2870; hi = 3820;
	}
	return std::clamp((mV - lo) / (hi - lo), 0.0f, 1.0f) * 100.0f;
}

static void initBattery(){
	adc_unit_t unit;
	ESP_ERROR_CHECK(adc_oneshot_io_to_channel(pins.batt, &unit, &battChan));
	adc_oneshot_unit_init_cfg_t u = {};
	u.unit_id = unit;
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adcUnit));
	adc_oneshot_chan_cfg_t c = {};
	c.atten = pins.vref >= 0 ? ADC_ATTEN_DB_2_5 : ADC_ATTEN_DB_12;
	c.bitwidth = ADC_BITWIDTH_12;
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adcUnit, battChan, &c));

	if(pins.vref >= 0){
		adc_cali_curve_fitting_config_t cc = {};
		cc.unit_id = unit;
		cc.chan = battChan;
		cc.atten = c.atten;
		cc.bitwidth = c.bitwidth;
		// Bit-Firmware aborts here; raw readings would give nonsense volts and could trip the
		// low-battery shutdown at every boot, so run without battery monitoring instead.
		if(adc_cali_create_scheme_curve_fitting(&cc, &battCali) != ESP_OK){
			printf("No ADC calibration in eFuse: battery monitoring off\n");
			battMonitor = false;
			return;
		}

		// Switch the 2.5 V reference onto the battery pin and measure the ADC offset (BatteryRev3::calibrate).
		const auto vref = (gpio_num_t) pins.vref;
		gpio_set_direction(vref, GPIO_MODE_OUTPUT);
		gpio_set_level(vref, 1);
		vTaskDelay(pdMS_TO_TICKS(100));
		float v = 0, total = 0;
		for(int i = 0; i < 20; i++){
			readBatt(v);
			if(i >= 10) total += v * 4;
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		battOffset = 2500 - total / 10;
		gpio_set_level(vref, 0);
		vTaskDelay(pdMS_TO_TICKS(100));
	}else{
		static const esp_efuse_desc_t offField = { EFUSE_BLK3, 0, 16 }; // BatteryRev1::getVoltOffset
		static const esp_efuse_desc_t* offBlob[] = { &offField, nullptr };
		int16_t off = 0;
		esp_efuse_read_field_blob(offBlob, &off, 16);
		battOffset = off;
	}
	sampleBatt();
}

// Small blinking red battery in the black band above the picture (cleared again when blinked off).
static void drawLowBattery(uint32_t now){
	const bool on = lowBattery && now / 500 % 2 == 0;
	const uint16_t red = __builtin_bswap16(0xF800), white = __builtin_bswap16(0xFFFF);
	constexpr int x0 = 114, w = 10, h = 6; // body; the 1 px terminal nub sits right of it
	static_assert(h <= Top, "the icon must fit in the band above the picture");
	for(int y = 0; y < h; y++)
		for(int x = 0; x <= w; x++){
			const bool edge = x == 0 || x == w - 1 || y == 0 || y == h - 1;
			uint16_t c = 0;
			if(on){
				if(x == w) c = y >= 2 && y <= 3 ? white : 0;
				else c = edge ? white : x <= 2 ? red : 0;
			}
			frame[y * Screen + x0 + x] = c;
		}
}

static uint8_t audio_read(uint16_t addr){ return apu.read(addr); }
static void audio_write(uint16_t addr, uint8_t val){ apu.write(addr, val); }

static uint8_t romRead(gb_s*, const uint_fast32_t addr){ return rom.read8(addr); }
static uint16_t romRead16(gb_s*, const uint_fast32_t addr){ return rom.read16(addr); }
static uint32_t romRead32(gb_s*, const uint_fast32_t addr){ return rom.read32(addr); }
static uint8_t ramRead(gb_s*, const uint_fast32_t addr){ return addr < cartRamSize ? cartRam[addr] : 0xFF; }
static void ramWrite(gb_s*, const uint_fast32_t addr, const uint8_t v){
	if(addr >= cartRamSize || cartRam[addr] == v) return;
	cartRam[addr] = v;
	ramWriteMs = millis();
	if(!ramDirty) ramDirtyMs = ramWriteMs;
	ramDirty = true;
}
static void onError(gb_s*, const gb_error_e e, const uint16_t addr){
	printf("Game Boy error %d at %04x\n", (int) e, addr);
}
static void drawLine(gb_s* g, const uint8_t* pixels, const uint_fast8_t y){
	if(g->cgb.cgbMode) scaler.line(pixels, y, g->cgb.fixPalette, 0x3F); // Game Boy Color: the game's colours
	else scaler.line(pixels, y, dmgPalette, 0x33); // shade and layer (OBJ0, OBJ1, BG)
}

// No usable game in the rom partition: say how to add one, then sleep like any idle Bit.
[[noreturn]] static void noGame(const char* why){
	printf("%s\n", why);
	lcd.setTextColor(TFT_WHITE);
	lcd.setTextDatum(lgfx::middle_center);
	lcd.drawString("No Game Boy game", 64, 44);
	lcd.drawString(why, 64, 60);
	lcd.drawString("Flash a game file", 64, 80);
	lcd.drawString("to the rom partition", 64, 92);
	backlightFade(blFull);
	vTaskDelay(pdMS_TO_TICKS(SleepMs));
	shutdown();
}

extern "C" void app_main(){
	gpio_force_unhold_all(); // pins stay latched from a previous low-battery shutdown otherwise

	const uint8_t rev = readRevision();
	pins = rev >= 3 ? PinsRev3 : PinsRev1;

	// Stock thresholds: Critical is below 5% at boot (no screen, straight off), below 1% while running.
	// shortcut: two fixed thresholds instead of stock's 8-level Hysteresis; same result for a slowly falling EMA.
	initBattery();
	if(battPercent() < 5) shutdown();

	const gpio_num_t keys[7] = { (gpio_num_t) pins.up, (gpio_num_t) pins.down, (gpio_num_t) pins.left, (gpio_num_t) pins.right,
		(gpio_num_t) pins.a, (gpio_num_t) pins.b, (gpio_num_t) pins.menu };
	static constexpr uint8_t keyBits[6] = { JOYPAD_UP, JOYPAD_DOWN, JOYPAD_LEFT, JOYPAD_RIGHT, JOYPAD_A, JOYPAD_B };
	uint64_t mask = 0;
	for(gpio_num_t k : keys) mask |= 1ULL << k;
	gpio_config_t io = {};
	io.pin_bit_mask = mask;
	io.mode = GPIO_MODE_INPUT;
	io.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&io));

	initBacklight(); // dark while the panel initializes
	initDisplay(rev);
	initBuzzer();

	const esp_partition_t* romPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "rom");
	esp_partition_mmap_handle_t romMap;
	if(!romPart || esp_partition_mmap(romPart, 0, romPart->size, ESP_PARTITION_MMAP_DATA, (const void**) &rom.data, &romMap) != ESP_OK)
		noGame("(no rom partition)");
	rom.size = romPart->size;
	if(!rom.headerOk()) noGame("(bad ROM header)");
	switch(gb_init(&gb, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr)){
		case GB_INIT_NO_ERROR: break;
		case GB_INIT_CARTRIDGE_UNSUPPORTED: noGame("(unsupported cart)");
		default: noGame("(bad ROM header)");
	}
	rom.clampToHeader();
	if(gb_get_save_size_s(&gb, &cartRamSize) != 0) cartRamSize = 0;
	if(cartRamSize){
		cartRam = (uint8_t*) calloc(1, cartRamSize);
		if(!cartRam) noGame("(not enough RAM)");
		if(hasBattery(rom.read8(0x147))) saveLoad(); // otherwise scratch RAM, lost at power-off as on a Game Boy
	}
	gb_init_lcd(&gb, drawLine);
	dmgColors(rom, dmgPalette);
	stateResume();
	char title[17];
	printf("Playing %s (%s, ROM %u KB, save RAM %u bytes)\n", gb_get_rom_name(&gb, title),
		gb.cgb.cgbMode ? "Game Boy Color" : "Game Boy", (unsigned) (rom.size / 1024), (unsigned) cartRamSize);

	lcd.startWrite(); // keep the bus so pushImageDMA returns while the transfer runs
	running = true;
	MenuButton menu;
	bool lit = false, dimmed = false;
	uint32_t tickTime = millis(), activityTime = tickTime, busyUs = 0, frameCount = 0;
	int64_t next = esp_timer_get_time();
	while(true){
		const uint32_t now = millis();
		bool active = false;
		uint8_t pressed = 0;
		for(int k = 0; k < 6; k++)
			if(gpio_get_level(keys[k]) == 0){ // active low, pulled up
				pressed |= keyBits[k];
				active = true;
			}
		const bool menuDown = gpio_get_level(keys[6]) == 0;
		pressed |= menu.update(menuDown, now);
		if(active || menuDown) activityTime = now;

		if(now - tickTime >= 100){
			tickTime = now;
			if(now - activityTime >= SleepMs) shutdown();
			if(lit && (now - activityTime >= DimMs) != dimmed){
				dimmed = !dimmed;
				if(dimmed){
					buzzerDuty(0); // the fade blocks for 200 ms
					backlightFade(blFull / 4);
				}
				else backlightSet(blFull); // instantly, the player is back
			}

			sampleBatt();
			lowBattery = battPercent() < 15;
			if(battPercent() < 1){
				buzzerDuty(0);
				backlightSet(blFull);
				lcd.fillScreen(TFT_BLACK);
				lcd.setTextColor(TFT_WHITE);
				lcd.setTextDatum(lgfx::middle_center);
				lcd.drawString("Battery critical,", 64, 56);
				lcd.drawString("shutting down", 64, 72);
				vTaskDelay(pdMS_TO_TICKS(6000));
				shutdown(); // stores the game's save first
			}

			// Games write their battery RAM in bursts: store once a burst has been over for a second, or
			// after 30 s for games that never stop writing it (the power switch skips shutdown()), at
			// most every 10 s (flash wear, and a store pauses the game for a moment).
			if(ramDirty && (now - ramWriteMs >= 1000 || now - ramDirtyMs >= 30000) && now - flushMs >= 10000){
				flushMs = now;
				saveFlush();
			}
		}

		const int64_t start = esp_timer_get_time();
		gb.direct.joypad = ~pressed;
		gb_run_frame_dualfetch(&gb);
		apu.tick(FrameUs);
		buzzerTone(apu.output());

		for(int y = 0; y < Scaler::H; y++){
			uint16_t* row = frame + (Top + y) * Screen;
			for(int x = 0; x < Scaler::W; x++) row[x] = __builtin_bswap16(scaler.out[y][x]); // panel order
		}
		drawLowBattery(now);
		// LovyanGFX waits for the previous transfer itself; the emulator then draws into the other buffer.
		lcd.pushImageDMA(0, 0, Screen, Screen, (const lgfx::swap565_t*) frame);
		frame = frame == frames[0] ? frames[1] : frames[0];
		if(!lit){
			lit = true;
			backlightFade(blFull);
		}

		// Shown by `idf.py monitor`: emulation + drawing time per second, in % of the frame time budget.
		busyUs += esp_timer_get_time() - start;
		if(++frameCount % 60 == 0){
			printf("CPU load: %d%%\n", (int) (busyUs * 100 / (60 * FrameUs)));
			busyUs = 0;
		}

		// Real Game Boy speed. Always yield at least a tick, so the idle task feeds the task watchdog;
		// when far behind (a save store, a slow scene) carry on from now instead of rushing to catch up.
		next += FrameUs;
		const int64_t wait = next - esp_timer_get_time();
		if(wait < -3 * FrameUs) next = esp_timer_get_time();
		vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(std::max<int64_t>(0, wait) / 1000))); // wait < 0: behind
	}
}
