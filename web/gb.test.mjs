// node --test web/: checks gb.js against the bundled ROM and broken variants of it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { checkRom, manifestFor } from "./gb.js";

const game = new Uint8Array(readFileSync(new URL("../roms/tobudx.gb", import.meta.url)));
const template = JSON.parse(readFileSync(new URL("./manifest.json", import.meta.url)));
const MB4 = template.rom_size;

// A copy of the bundled ROM with header bytes changed, its checksum fixed again.
function variant(changes) {
	const rom = game.slice();
	for (const [at, value] of Object.entries(changes)) rom[at] = value;
	let sum = 0;
	for (let i = 0x134; i <= 0x14C; i++) sum = (sum - rom[i] - 1) & 0xFF;
	rom[0x14D] = sum;
	return rom;
}

test("accepts the bundled game", () => {
	assert.deepEqual(checkRom(game, MB4), { title: "TOBUDX", size: game.length });
});

test("a file shorter than its header's ROM size is padded to it", () => {
	// the header says 256 KB (code 3): flashing 32 KB would leave the old game's bytes after it
	assert.equal(checkRom(game.slice(0, 0x8000), MB4).size, 0x40000);
	assert.throws(() => checkRom(variant({ 0x148: 8 }), MB4), /8\.0 MB; the Bit holds up to 4 MB/);
});

test("rejects what the firmware would", () => {
	assert.throws(() => checkRom(game.slice(0, 0x100), MB4), /too small/);
	assert.throws(() => checkRom(game, 0x10000), /holds up to/);
	assert.throws(() => checkRom(new Uint8Array(0x8000), MB4), /no Nintendo logo/);
	const damaged = game.slice();
	damaged[0x134] ^= 1;
	assert.throws(() => checkRom(damaged, MB4), /checksum/);
	assert.throws(() => checkRom(variant({ 0x147: 0x22 }), MB4), /cartridge type \(0x22\)/);
	assert.throws(() => checkRom(variant({ 0x148: 9 }), MB4), /unknown ROM or RAM size/);
	assert.throws(() => checkRom(variant({ 0x149: 6 }), MB4), /unknown ROM or RAM size/);
});

test("Game Boy Color: dual-mode and Color-only games are fine, the flag isn't part of the title", () => {
	assert.equal(checkRom(variant({ 0x143: 0x80 }), MB4).title, "TOBUDX");
	assert.equal(checkRom(variant({ 0x143: 0xC0 }), MB4).title, "TOBUDX");
	const full = {}; // a title filling all 16 bytes: original games use 0x143 too, Color ones not
	for (let i = 0x13A; i <= 0x142; i++) full[i] = 0x42;
	assert.equal(checkRom(variant({ ...full, 0x143: 0x41 }), MB4).title, "TOBUDXBBBBBBBBBA");
	assert.equal(checkRom(variant({ ...full, 0x143: 0x80 }), MB4).title, "TOBUDXBBBBBBBBB");
	assert.equal(checkRom(variant({ 0x134: 0x01 }), MB4).title, "?OBUDX"); // unprintable shown, not trusted
});

test("manifests put the game at the rom partition, with or without the firmware", () => {
	const romOffset = template.builds[0].parts.find((p) => p.offset !== 0).offset;
	const page = "https://example.org/CGB-Bit/";
	const only = manifestFor(template, "blob:x", "TOBUDX", false, page);
	assert.deepEqual(only.builds[0].parts, [{ path: "blob:x", offset: romOffset }]);
	assert.equal(only.name, `${template.name}: TOBUDX`);
	const full = manifestFor(template, "blob:x", "TOBUDX", true, page);
	assert.deepEqual(full.builds[0].parts.map((p) => p.offset), [0, romOffset]);
	// served from a blob: URL, so every part path must already be absolute
	for (const p of full.builds[0].parts) assert.doesNotThrow(() => new URL(p.path));
	assert.equal(full.builds[0].parts[0].path, "https://example.org/CGB-Bit/cgb-bit-firmware.bin");
	assert.equal(full.builds[0].chipFamily, "ESP32-S3");
});

test("installs ask before erasing", () => {
	// Without it ESP Web Tools erases the whole chip on every install of a device without Improv
	// Serial (CGB-Bit has none): saves gone, and "Replace the game only" would leave no firmware.
	assert.equal(template.new_install_prompt_erase, true);
	for (const withFirmware of [false, true]) assert.equal(manifestFor(template, "blob:x", "T", withFirmware, "https://x/").new_install_prompt_erase, true);
});
