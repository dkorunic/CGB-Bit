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
