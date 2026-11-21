// Checks a Game Boy or Game Boy Color ROM the way CGB-Bit's firmware (Walnut-CGB's gb_init) will,
// before the installer flashes it, and builds the ESP Web Tools manifests for it. Tested by gb.test.mjs.

const NINTENDO_LOGO = [
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
	0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
	0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
];

// Cartridge types (header byte 0x147) Walnut-CGB runs: cart_mbc[] != -1 in walnut_cgb.h.
const SUPPORTED_CART_TYPES = new Set([
	0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x09, 0x0B, 0x0C, 0x0D,
	0x0F, 0x10, 0x11, 0x12, 0x13, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
]);

/**
 * @param {Uint8Array} rom the whole .gb file
 * @param {number} maxSize the rom partition's size
 * @returns {{title: string, size: number}} size: what to flash (the file, padded with 0xFF up to the
 *   ROM size its header declares, which the firmware reads up to), or throws an Error whose message
 *   explains the problem to the player
 */
export function checkRom(rom, maxSize) {
	if (rom.length < 0x150) throw new Error("This file is too small to be a Game Boy game.");
	if (NINTENDO_LOGO.some((b, i) => rom[0x104 + i] !== b)) throw new Error("This isn't a Game Boy game: its header has no Nintendo logo.");
	let sum = 0;
	for (let i = 0x134; i <= 0x14C; i++) sum = (sum - rom[i] - 1) & 0xFF;
	if (sum !== rom[0x14D]) throw new Error("The game's header checksum doesn't match: the file is damaged or modified.");
	const type = rom[0x147];
	if (!SUPPORTED_CART_TYPES.has(type)) throw new Error(`This game's cartridge type (0x${type.toString(16).padStart(2, "0")}) isn't supported.`);
	if (rom[0x148] > 8 || rom[0x149] > 5) throw new Error("The game's header gives an unknown ROM or RAM size.");
	// A shorter file is padded: flashing only the file would leave the previous game's bytes there.
	const size = Math.max(rom.length, 0x8000 << rom[0x148]);
	if (size > maxSize) throw new Error(`This game is ${(size / 1048576).toFixed(1)} MB; the Bit holds up to ${maxSize / 1048576} MB.`);

	const end = rom[0x143] & 0x80 ? 0x143 : 0x144; // Color games use the title's last byte as a flag
	let title = "";
	for (let i = 0x134; i < end && rom[i] !== 0; i++) title += rom[i] >= 0x20 && rom[i] < 0x7F ? String.fromCharCode(rom[i]) : "?";
	return { title: title.trim() || "Untitled", size };
}

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
	for (let k = 0; k < 8; k++) n = n & 1 ? 0xEDB88320 ^ (n >>> 1) : n >>> 1;
	return n >>> 0;
});
/** CRC-32 (zlib's), which BPS patches carry for the game, the result and themselves. */
export function crc32(bytes) {
	let c = 0xFFFFFFFF;
	for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >>> 8);
	return (c ^ 0xFFFFFFFF) >>> 0;
}

/**
 * A ROM patch (translation, bug fix, hack) applied to the game in the browser, before checkRom.
 * IPS: records of bytes at offsets, possibly past the end (the game grows). BPS: rebuilds the
 * patched game from the original and checks both against their CRC-32s, so a patch for another
 * version of the game is refused rather than producing a broken one.
 * @param {Uint8Array} rom the original game
 * @param {Uint8Array} patch the .ips or .bps file
 * @returns {Uint8Array} the patched game; or throws an Error explaining the problem to the player
 */
export function applyPatch(rom, patch) {
	const magic = String.fromCharCode(...patch.subarray(0, 5));
	if (magic === "PATCH") return applyIps(rom, patch);
	if (magic.startsWith("BPS1")) return applyBps(rom, patch);
	throw new Error("This patch isn't an IPS or BPS file.");
}

function applyIps(rom, patch) {
	const damaged = () => new Error("The patch file is damaged (it ends too early).");
	let out = rom.slice();
	const write = (at, bytes) => {
		if (at + bytes.length > out.length) {
			const grown = new Uint8Array(at + bytes.length);
			grown.set(out);
			out = grown;
		}
		out.set(bytes, at);
	};
	let p = 5;
	const take = (n) => {
		if (p + n > patch.length) throw damaged();
		let v = 0;
		for (let i = 0; i < n; i++) v = v * 256 + patch[p++];
		return v;
	};
	for (;;) {
		if (p + 3 <= patch.length && patch[p] === 0x45 && patch[p + 1] === 0x4F && patch[p + 2] === 0x46) { // "EOF"
			p += 3;
			if (p + 3 <= patch.length) out = out.slice(0, take(3)); // a truncation offset follows some
			return out;
		}
		const at = take(3), size = take(2);
		if (size) {
			if (p + size > patch.length) throw damaged();
			write(at, patch.subarray(p, p + size));
			p += size;
		} else {
			const run = take(2), value = take(1);
			write(at, new Uint8Array(run).fill(value));
		}
	}
}

function applyBps(rom, patch) {
	const damaged = new Error("The patch file is damaged.");
	if (patch.length < 4 + 12) throw damaged;
	const u32 = (at) => (patch[at] | patch[at + 1] << 8 | patch[at + 2] << 16 | patch[at + 3] << 24) >>> 0;
	const end = patch.length - 12;
	if (crc32(patch.subarray(0, end + 8)) !== u32(end + 8)) throw damaged;
	if (crc32(rom) !== u32(end)) throw new Error("This patch is for another version of the game (or another game): pick the file it was made for.");
	let p = 4;
	const number = () => { // BPS varint, without 32-bit overflow
		let value = 0, shift = 1;
		for (;;) {
			if (p >= end) throw damaged;
			const x = patch[p++];
			value += (x & 0x7F) * shift;
			if (x & 0x80) return value;
			shift *= 128;
			value += shift;
		}
	};
	const sourceSize = number(), targetSize = number();
	const metadata = number(); // not `p += number()`: that adds to p as it was before number() moved it
	p += metadata;
	if (sourceSize !== rom.length || targetSize > 64 << 20) throw damaged;
	const out = new Uint8Array(targetSize);
	let o = 0, sourceAt = 0, targetAt = 0;
	const offset = () => { const v = number(); return (v & 1 ? -1 : 1) * Math.floor(v / 2); };
	while (p < end) {
		const action = number(), length = Math.floor(action / 4) + 1;
		if (o + length > targetSize) throw damaged;
		switch (action & 3) {
			case 0: // SourceRead: the original's bytes at the same place
				if (o + length > rom.length) throw damaged;
				out.set(rom.subarray(o, o + length), o);
				break;
			case 1: // TargetRead: new bytes from the patch
				if (p + length > end) throw damaged;
				out.set(patch.subarray(p, p + length), o);
				p += length;
				break;
			case 2: // SourceCopy: the original's bytes from elsewhere
				sourceAt += offset();
				if (sourceAt < 0 || sourceAt + length > rom.length) throw damaged;
				out.set(rom.subarray(sourceAt, sourceAt + length), o);
				sourceAt += length;
				break;
			case 3: // TargetCopy: earlier output, byte by byte (the ranges may overlap)
				targetAt += offset();
				if (targetAt < 0 || targetAt >= o) throw damaged;
				for (let i = 0; i < length; i++) out[o + i] = out[targetAt++];
				break;
		}
		o += length;
	}
	if (o !== targetSize || crc32(out) !== u32(end + 4)) throw damaged;
	return out;
}

/**
 * ESP Web Tools manifest flashing `gameUrl` into the rom partition, alone or with the firmware.
 * @param {object} template the site's manifest.json: firmware at offset 0, the bundled game at the rom offset
 * @param {string} base the page's URL: the firmware path is made absolute against it, so the manifest
 *   works from a blob: URL whatever base ESP Web Tools resolves its parts against
 */
export function manifestFor(template, gameUrl, title, withFirmware, base) {
	const parts = template.builds[0].parts;
	const firmware = parts.find((p) => p.offset === 0);
	const game = parts.find((p) => p.offset !== 0);
	return {
		...template,
		name: `${template.name}: ${title}`,
		builds: [{
			chipFamily: template.builds[0].chipFamily,
			parts: [...(withFirmware ? [{ ...firmware, path: new URL(firmware.path, base).href }] : []), { path: gameUrl, offset: game.offset }],
		}],
	};
}
