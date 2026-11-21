// node --test web/: checks gb.js against the bundled ROM and broken variants of it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { applyPatch, checkRom, crc32, manifestFor } from "./gb.js";

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

// Patches. BPS ones are built here by a minimal encoder: varints, actions, CRC-32 footer.
const ascii = (text) => [...text].map((c) => c.charCodeAt(0));
const varint = (n) => {
	const out = [];
	for (;;) {
		const x = n % 128;
		n = Math.floor(n / 128);
		if (n === 0) return [...out, 0x80 | x];
		out.push(x);
		n--;
	}
};
const le32 = (v) => [v & 0xFF, v >>> 8 & 0xFF, v >>> 16 & 0xFF, v >>> 24];
const action = (kind, length) => varint((length - 1) * 4 + kind);
const relative = (n) => varint(Math.abs(n) * 2 + (n < 0 ? 1 : 0));
const sourceRead = (length) => action(0, length);
const targetRead = (bytes) => [...action(1, bytes.length), ...bytes];
const sourceCopy = (length, move) => [...action(2, length), ...relative(move)];
const targetCopy = (length, move) => [...action(3, length), ...relative(move)];
function bps(source, target, actions) {
	const body = [...ascii("BPS1"), ...varint(source.length), ...varint(target.length), ...varint(0), ...actions.flat(),
		...le32(crc32(source)), ...le32(crc32(target))];
	return new Uint8Array([...body, ...le32(crc32(body))]);
}
// The bundled game retitled "PATCHED": what each patch below makes of it.
const patched = variant(Object.fromEntries(ascii("PATCHED\0").map((c, i) => [0x134 + i, c])));

test("CRC-32 is zlib's", () => {
	assert.equal(crc32(new Uint8Array(ascii("123456789"))), 0xCBF43926);
});

test("applies IPS patches", () => {
	const changed = [];
	for (let i = 0; i < game.length; i++) if (patched[i] !== game[i]) changed.push(i);
	const record = (at, bytes) => [at >> 16, at >> 8 & 0xFF, at & 0xFF, bytes.length >> 8, bytes.length & 0xFF, ...bytes];
	const ips = new Uint8Array([...ascii("PATCH"), ...changed.flatMap((at) => record(at, [patched[at]])), ...ascii("EOF")]);
	assert.equal(checkRom(applyPatch(game, ips), MB4).title, "PATCHED");

	const small = new Uint8Array([1, 2, 3, 4]);
	const grow = new Uint8Array([...ascii("PATCH"), ...record(6, [7, 8]), 0, 0, 1, 0, 0, 0, 3, 9, ...ascii("EOF")]); // + run of 3 nines at 1
	assert.deepEqual([...applyPatch(small, grow)], [1, 9, 9, 9, 0, 0, 7, 8]);
	const truncate = new Uint8Array([...ascii("PATCH"), ...ascii("EOF"), 0, 0, 2]);
	assert.deepEqual([...applyPatch(small, truncate)], [1, 2]);
	assert.throws(() => applyPatch(small, new Uint8Array([...ascii("PATCH"), 0, 0, 1, 0, 5, 1])), /damaged/);
	assert.equal(small.join(), "1,2,3,4"); // the original is left alone
});

test("applies BPS patches, only to the game they were made for", () => {
	const title = [...patched.subarray(0x134, 0x14E)];
	const patch = bps(game, patched, [sourceRead(0x134), targetRead(title), sourceRead(game.length - 0x14E)]);
	assert.equal(checkRom(applyPatch(game, patch), MB4).title, "PATCHED");

	// all four actions: copies from elsewhere in the original, and overlapping copies of the output
	const source = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), target = new Uint8Array([5, 6, 7, 8, 9, 9, 9, 9, 1, 2]);
	const moves = bps(source, target, [sourceCopy(4, 4), targetRead([9]), targetCopy(3, 4), sourceCopy(2, -8)]);
	assert.deepEqual(applyPatch(source, moves), target);

	assert.throws(() => applyPatch(patched, patch), /for another version of the game/);
	const damaged = patch.slice();
	damaged[10] ^= 1;
	assert.throws(() => applyPatch(game, damaged), /damaged/);
	assert.throws(() => applyPatch(game, new Uint8Array(ascii("UPS1xxxxxxxxxxxx"))), /isn't an IPS or BPS file/);
});
