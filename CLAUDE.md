# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

CGB-Bit: ESP-IDF firmware (ESP32-S3, no PSRAM, 512 KB SRAM, 8 MB flash) that runs Game Boy and Game
Boy Color games on a CircuitMess Bit, using the Walnut-CGB emulator (git submodule `walnut-cgb/`,
never edited in place; see "Walnut-CGB patches"). README.md covers features, controls and flashing for users.

## Commands

Firmware (ESP-IDF 5.x; tested with v5.4.1 in `~/esp/esp-idf`):

```shell
. ~/esp/esp-idf/export.sh
idf.py build                               # GB_ROM defaults to roms/tobudx.gb
idf.py -DGB_ROM=path/game.gbc build        # another bundled game (≤ 4 MB; relative to the project dir)
idf.py -p /dev/ttyACM0 flash monitor       # monitor prints "CPU load: N%" every second
idf.py merge-bin -o cgb-bit-merged.bin     # -> build/cgb-bit-merged.bin, the release image
```

`GB_ROM` is a CMake cache variable: an existing `build/` keeps the old path (delete `build/` if it
points at a missing file).

PC tools (no ESP-IDF needed; run them from a plain shell, the ESP-IDF shell's Python lacks Pillow):

```shell
tools/smoke_test.sh [game.gbc]       # front-end unit checks + 1 emulated minute under ASan/UBSan
node --test web/gb.test.mjs          # installer ROM checks + manifests
node --test --test-name-pattern "accepts" web/gb.test.mjs   # a single web test
tools/make_demo.sh                   # regenerates demo.gif (needs Pillow)
web/build.sh <out dir> build/cgb-bit-merged.bin <version>   # assemble the installer site
```

The smoke test's unit checks (`testScaler`, `testApu`, `testRom`, `testSaves`, `testCartRam`, `testDmgColors`, `testMenu` in
`tools/smoke_test.cpp`) all run on every invocation; there is no per-check selection. After the
emulated minute it also round-trips a save state into a fresh `gb_s` and checks 600 more frames
match exactly.

## Architecture

- `main/frontend.h`: all emulator-facing logic with no ESP-IDF dependency (the box-filter `Scaler`,
  the `ToneApu` sound tracker, the `MenuButton` Start/Select logic, `dmgColors`, the save-state helpers). It is shared
  by the firmware and both PC tools, so a change here is exercised by the smoke test and shows up in
  the demo GIF. Include `walnut_cgb.h` before it.
- `main/main.cpp`: everything hardware: Bit pins (two revisions, chosen by the efuse revision),
  ST7735S via LovyanGFX with double-buffered DMA, buzzer and backlight on LEDC, battery ADC,
  deep-sleep shutdown, save storage, frame pacing (16,743 µs per frame). The Bit hardware code is
  carried over from the sibling Anarch-Bit project; keep the two consistent when fixing it.
- `tools/smoke_test.cpp` and `tools/demo.cpp` duplicate the firmware's small Walnut-CGB callbacks
  (`drawLine`, the ROM readers, which wrap `frontend.h`'s `Rom`); keep them in step with main.cpp.

Walnut-CGB patches: `tools/patch_walnut.sh <out dir>` copies `walnut-cgb/walnut_cgb.h` to
`<out dir>` and applies `patches/*.diff` in order with `patch -F0` (no fuzz: a patch that no longer
applies exactly fails the build). The firmware includes the copy in `build/walnut` (a custom
command in `main/CMakeLists.txt`, re-run when a patch or the header changes); both PC tools patch
into their temp dir. Each patch is `git format-patch` output of an upstream PR commit under a
provenance header; the upstream file has CRLF line endings, so the diffs do too (keep them
byte-exact). Current: `01-mbc2-cart-ram.diff` (PR #7, MBC2 RAM zeroed by `gb_init` because MBC2
headers declare RAM size 0) and `02-mbc5-rumble-cart-ram.diff` (PR #8, `cart_ram[]` wrong for
0x1D/0x1E and the rumble motor bit 3 selecting RAM banks 8..15). `testCartRam` runs tiny
hand-assembled ROMs for both and fails without the patches. When upstream merges a PR: bump the
submodule, delete the patch (and the patches/ mechanism once empty).

Emulator integration (Walnut-CGB specifics):
- `gb_init` takes 8-, 16- and 32-bit ROM readers, all `frontend.h`'s `Rom`: the ROM is a
  memory-mapped flash partition, which only allows aligned word loads, so `read16/32` use them for
  aligned addresses and fall back to bytes otherwise (the smoke test checks both paths).
- Frames run with `gb_run_frame_dualfetch()` (the plain `gb_run_frame` uses the slower core).
- `drawLine` gets palette indices: in Game Boy Color mode (`gb->cgb.cgbMode`) they index
  `gb->cgb.fixPalette` (64 RGB565 entries, mask 0x3F); otherwise `pixel & 0x33` (shade, plus the
  OBJ0/OBJ1/BG layer of the 12-colour mode) indexes `dmgColors`' table: the Game Boy Color boot
  ROM's palette for the title (`main/cgb_palettes.h`, generated from SameBoy's `cgb_boot.asm`;
  regenerate rather than edit). `Scaler::out` is native RGB565; the firmware byte-swaps for the panel.
- `Scaler` is the hot spot after the emulator (it was half the frame time before it packed R, G, B
  into one 32-bit word and filtered each line horizontally once). Keep it bit-exact: `demo.gif`'s
  md5 must not change for a pure speed change. Its two stages run on the two cores in the
  firmware: `drawLine` (core 0, emulator) runs `Scaler::filter` into a 4-slot ring and queues each
  finished 5-line group; `scaleTask` (core 1) runs `Scaler::emit<true>` straight into the DMA frame
  buffer in panel byte order. The main loop pushes a frame only once `frameScaled` says its last
  group is in (no push while the game has the LCD off). All LCD/SPI calls stay on core 0. The PC
  tools use `Scaler::line` (both stages, native order); the smoke test checks the split matches.
  `CPU load` prints core 0's share and the scaler's share on core 1.
- Save states: Walnut-CGB keeps all mutable state in `gb_s` (its statics are const tables), so a
  state is `gb_s` + `ToneApu` + cartridge RAM (`stateParts`). `loadState` keeps this boot's
  callback pointers; the header carries the firmware's ELF SHA-256, so any rebuild invalidates
  states (gb_s layout, function addresses).
- Sound: `ENABLE_SOUND` routes register reads/writes 0xFF10..0xFF3F to `audio_read`/`audio_write`
  (backed by `ToneApu`). Walnut-CGB's 16/32-bit reads call `audio_read` for `addr + 1..3`, which
  can go past 0xFF3F, so `ToneApu` bounds-checks.
- `gb_init` indexes its size tables with header bytes 0x148/0x149 unchecked; `Rom::headerOk`
  rejects codes > 8 / > 5 and a missing Nintendo logo first. Keep the firmware checks and
  `web/gb.js`'s checks (logo, checksum, cartridge types from `cart_mbc[]`, sizes) in agreement.
- The firmware reads a ROM up to the size its header declares (`Rom::clampToHeader`), but writing
  a game only replaces as many bytes as the file has: a shorter file must be padded with 0xFF to
  the declared size. `web/gb.js` returns that size and the page pads; `main/CMakeLists.txt` refuses
  a short `GB_ROM`.

Flash layout (`partitions.csv`) and its coupling:
- `rom` (data 0x40) at 0x110000, 4 MB: the game, written by `esptool_py_flash_to_partition` in
  `main/CMakeLists.txt` and therefore also inside the merged image.
- `saves` (data 0x41), 264 KB: two slots of 0x21000, each a header sector (`SaveHeader` in
  `frontend.h`: magic `GBS2`, sequence number, the ROM's 0x134..0x14F header, size) then the
  cartridge RAM. Stores go to the slot not holding the current save, header written last, so a
  power cut keeps the previous save; `newestSave` picks the newer valid slot for this ROM. Only
  battery cartridge types (`hasBattery`) are saved. No install path writes this partition unless
  the user ticks "Erase device".
- `state` (data 0x42), 192 KB: `StateHeader` sector, then the state; `StatePartSize` in main.cpp
  must match (a static_assert checks the largest state fits). `shutdown()` stores it after
  `saveFlush()` (only once the game runs); the next boot erases the header before loading, so it
  resumes once. A resumed cartridge RAM newer than the battery save is marked dirty and stored.
- The `rom` offset and size are repeated in `web/manifest.json`. `web/build.sh` cuts the merged
  image at that offset into `cgb-bit-firmware.bin` and fails if the bundled ROM isn't found there,
  so moving partitions means updating `web/manifest.json`.

Browser installer (`web/`): ESP Web Tools 10.4.0, downloaded by `web/build.sh` from npm, checked
against a pinned SHA-512 and served from the site (no CDN). `"new_install_prompt_erase": true` in
`web/manifest.json` is essential: CGB-Bit has no Improv Serial, and without it ESP Web Tools erases
the whole chip on every install (wiping saves, and leaving no firmware after "Replace the game
only"). For a player's own game the page builds
a manifest as a `blob:` URL pointing at the picked file ("Replace the game only" = the game at the
`rom` offset; "Install CGB-Bit with this game" = firmware part + game). `window.gbBit.useFile` is
exposed for tests.

CI (`.github/workflows/build.yml`): tests job (smoke test + node test), firmware job in a
digest-pinned `espressif/idf:v5.4.1` container, and on `v*` tags a release job (the only job with
`contents: write`) and a GitHub Pages deploy job; the `github-pages` environment must allow `v*`
tags (README, Development). All actions are pinned to commit SHAs with the
version in a comment; Dependabot updates them.

## Conventions

- `demo.cpp`'s input script is frame-counted for Tobu Tobu Girl Deluxe (`roms/tobudx.gb`); changing
  the bundled game or anything affecting gameplay timing may need it retuned (check frames with a
  contact sheet before regenerating `demo.gif`).
- Bundled ROMs must be redistributable; `roms/README.md` records source, licences and SHA-256.
- Repository code is CC0; third-party parts keep their licences (see README "License").
