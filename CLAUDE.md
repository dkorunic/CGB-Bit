# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

CGB-Bit: ESP-IDF firmware (ESP32-S3, no PSRAM, 512 KB SRAM, 8 MB flash) that runs Game Boy and Game
Boy Color games on a CircuitMess Bit, using the Walnut-CGB emulator (git submodule `walnut-cgb/`,
never edited in place; see "Walnut-CGB patches"). README.md covers features, controls and flashing
for users.

## Commands

Firmware (ESP-IDF 5.x; tested with v5.4.1 in `~/esp/esp-idf`):

```shell
. ~/esp/esp-idf/export.sh
idf.py build                           # GB_ROM defaults to roms/tobudx.gb
idf.py -DGB_ROM=path/game.gbc build    # another bundled game (≤ 4 MB; relative to the project dir)
idf.py -p /dev/ttyACM0 flash monitor   # prints "CPU load: N% (scaler on core 1: M%)" every second
idf.py merge-bin -o cgb-bit-merged.bin # -> build/cgb-bit-merged.bin, the release image
```

`GB_ROM` is a CMake cache variable: an existing `build/` keeps the old path (delete `build/` if it
points at a missing file).

PC tools (no ESP-IDF needed; run them from a plain shell, the ESP-IDF shell's Python lacks Pillow):

```shell
tools/smoke_test.sh [game.gbc]       # front-end unit checks + 1 emulated minute under ASan/UBSan
node --test web/gb.test.mjs          # installer ROM checks + manifests
node --test --test-name-pattern "accepts" web/gb.test.mjs   # a single web test
tools/make_demo.sh                   # regenerates demo.gif (needs Pillow and gifsicle)
web/build.sh <out dir> build/cgb-bit-merged.bin <version>   # assemble the installer site
```

The smoke test's unit checks (`testScaler`, `testApu`, `testRom`, `testSaves`, `testCartRam`,
`testDmgColors`, `testMenu` in `tools/smoke_test.cpp`) all run on every invocation; there is no
per-check selection. After the emulated minute it also round-trips a save state into a fresh
`gb_s` and checks that 600 more frames match exactly (picture, sound, cartridge RAM).

## Architecture

- `main/frontend.h`: all emulator-facing logic with no ESP-IDF dependency: the box-filter
  `Scaler`, the `ToneApu` sound tracker, the `MenuButton` Start/Select logic, `Rom`, the battery
  save slots (`SaveHeader`, `storeSlot`, `newestSave`), `dmgColors`, the save-state helpers
  (`StateHeader`, `stateParts`, `loadState`). It is shared by the firmware and both PC tools, so a
  change here is exercised by the smoke test and shows up in the demo GIF. Include `walnut_cgb.h`
  before it.
- `main/main.cpp`: everything hardware: Bit pins (two revisions, chosen by the efuse revision),
  ST7735S via LovyanGFX with double-buffered DMA, the scaler task on core 1, buzzer and backlight
  on LEDC, battery ADC, deep-sleep shutdown, battery saves and suspend/resume, frame pacing
  (16,743 µs per frame). The Bit hardware code is carried over from the sibling Anarch-Bit
  project; keep the two consistent when fixing it.
- `main/cgb_palettes.h`: generated from SameBoy's `BootROMs/cgb_boot.asm` (Expat licence, credited
  in README); regenerate rather than edit.
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
hand-assembled ROMs for both and fails without the patches. Dependabot opens a PR when the
submodule moves; when upstream has merged a PR, that patch stops applying: bump the submodule,
delete the patch (and the patches/ mechanism once empty).

Emulator integration (Walnut-CGB specifics):
- `gb_init` takes 8-, 16- and 32-bit ROM readers, all `frontend.h`'s `Rom`: the ROM is a
  memory-mapped flash partition, which only allows aligned word loads, so `read16/32` use them for
  aligned addresses and fall back to bytes otherwise (the smoke test checks both paths).
- Frames run with `gb_run_frame_dualfetch()` (the plain `gb_run_frame` uses the slower core).
- `drawLine` gets palette indices: in Game Boy Color mode (`gb->cgb.cgbMode`) they index
  `gb->cgb.fixPalette` (64 RGB565 entries, mask 0x3F); otherwise `pixel & 0x33` (shade, plus the
  OBJ0/OBJ1/BG layer of the 12-colour mode) indexes `dmgColors`' table: the Game Boy Color boot
  ROM's palette for the title (Nintendo licensee + title checksum + 4th letter), else its default.
  Games can change palettes between lines, so colours are resolved per line, in `drawLine`.
- Sound: `ENABLE_SOUND` routes register reads/writes 0xFF10..0xFF3F to `audio_read`/`audio_write`
  (backed by `ToneApu`). Walnut-CGB's 16/32-bit reads call `audio_read` for `addr + 1..3`, which
  can go past 0xFF3F, so `ToneApu` bounds-checks.
- Save states: Walnut-CGB keeps all mutable state in `gb_s` (its statics are const tables), so a
  state is `gb_s` + `ToneApu` + cartridge RAM (`stateParts`). `loadState` keeps this boot's
  callback pointers; the header carries the firmware's ELF SHA-256, so any rebuild invalidates
  states (gb_s layout, function addresses).
- `gb_init` indexes its size tables with header bytes 0x148/0x149 unchecked; `Rom::headerOk`
  rejects codes > 8 / > 5 and a missing Nintendo logo first. Keep the firmware checks and
  `web/gb.js`'s checks (logo, checksum, cartridge types from `cart_mbc[]`, sizes) in agreement.
- The firmware reads a ROM up to the size its header declares (`Rom::clampToHeader`), but writing
  a game only replaces as many bytes as the file has: a shorter file must be padded with 0xFF to
  the declared size. `web/gb.js` returns that size and the page pads; `main/CMakeLists.txt` refuses
  a short `GB_ROM`.

Scaler and the two cores: `Scaler` is 160x144 → 128x115, an exact 5:4 box filter per channel, with
R, G, B packed into one 32-bit word so one multiply weights all three. Keep it bit-exact: for a
pure speed change `demo.gif`'s md5 must not change. In the firmware, `drawLine` (core 0, with the
emulator) only runs `Scaler::colours` into a 4-slot ring and queues each finished 5-line group
(`ScaleJob`); `scaleTask` (pinned to core 1) runs `Scaler::filter` and `Scaler::emit<true>`
straight into the DMA frame buffer in panel byte order. Slots are taken and freed in order
(`ringFree` counts free ones). The main loop pushes a frame only once `frameScaled` says its last
group is in (no push while the game has the LCD off: the screen keeps the last frame); all
LCD/SPI calls stay on core 0. The PC tools use `Scaler::line` (all stages, native byte order) and
the smoke test checks that the split matches it.

## Performance

`CPU load` is core 0's busy time per second as a share of the frame budget (over 100% = the game
runs slow); the second number is `scaleTask` on core 1. Measured on a Bit 3 with Tobu Tobu Girl
Deluxe, steady play (heaviest stretch in brackets):

| Build | Core 0 | Core 1 |
|---|---|---|
| v0.1.0 (scaler: 12 multiplies per pixel) | ~133% (150%) | idle |
| v0.1.1 (packed channels, horizontal pass once per line) | ~94% (106%) | idle |
| a3855a8 (vertical pass on core 1) | ~83% (98%) | ~10% |
| current (whole scaler on core 1, only colour lookup on core 0) | ~77% (90%) | ~22% |

Nearly all that is left on core 0 is the emulator. Already tuned in `sdkconfig.defaults`: 240 MHz,
`-O2`, QIO flash at 80 MHz, 32 KB instruction / 64 KB data cache. Measure a change on the PC
first (time the stage in a small harness around `frontend.h`), then on the device.

Tried and not kept: running the emulator's hot code from internal RAM. ~43 KB (`__gb_step_cpu`
alone is 30 KB; `gb_run_frame_dualfetch` is inlined into `app_main`) gave ~73% (87%), i.e. 4-5
points, for ~45 KB of internal RAM (heap left for cartridge RAM: 131 KB → 86 KB). Walnut-CGB has
no attribute hook, and an ESP-IDF linker fragment can't target `libmain.a` (ldgen fails to parse
the `LINK_ONCE_DISCARD` flag of the C++ COMDAT groups), so it was done in a `PRE_LINK` step: objcopy
renamed the functions' `.text.F`/`.literal.F` sections in `main.cpp.obj` to `.iram1.F` and
`.iram1.F.literal`, exactly the names `IRAM_ATTR` produces. Worth revisiting if a game needs the
headroom.

## Flash layout (`partitions.csv`) and its coupling

- `rom` (data 0x40) at 0x110000, 4 MB: the game, written by `esptool_py_flash_to_partition` in
  `main/CMakeLists.txt` and therefore also inside the merged image.
- `saves` (data 0x41), 264 KB: two slots of 0x21000, each a header sector (`SaveHeader` in
  `frontend.h`: magic `GBS2`, sequence number, the ROM's 0x134..0x14F header, size) then the
  cartridge RAM. Stores go to the slot not holding the current save, header written last, so a
  power cut keeps the previous save; `newestSave` picks the newer valid slot for this ROM. Only
  battery cartridge types (`hasBattery`) are saved. The header also carries the MBC3 clock
  (`saveRtc`/`loadRtc`; Walnut-CGB starts it at day 0 on every cold boot), in fields appended after
  `size` with a marker byte, so headers from older builds (erased 0xFF there) still load. No
  install path writes this partition unless the user ticks "Erase device".
- `state` (data 0x42), 192 KB: `StateHeader` sector, then the state; `StatePartSize` in main.cpp
  must match (a static_assert checks the largest state fits). `shutdown()` stores it after
  `saveFlush()` (only once the game runs); the next boot erases the header before loading, so it
  resumes once. `resumeAction` decides first: a state older than the game's newest battery save
  (stored since, e.g. on another firmware build, which ignores the state) is erased unused, or its
  older cartridge RAM would replace the newer save; a resumed RAM newer than the save is marked
  dirty and stored. The smoke test covers the cases.
  The partition table is in the firmware part (0x8000), so updating with `cgb-bit.bin` alone
  (0x10000) keeps an older table: no `state` partition, resume off (logged), nothing breaks.
- The `rom` offset and size are repeated in `web/manifest.json`. `web/build.sh` cuts the merged
  image at that offset into `cgb-bit-firmware.bin` and fails if the bundled ROM isn't found there,
  so moving partitions means updating `web/manifest.json`.

## Browser installer and CI

Browser installer (`web/`): ESP Web Tools 10.4.0, downloaded by `web/build.sh` from npm, checked
against a pinned SHA-512 and served from the site (no CDN). `"new_install_prompt_erase": true` in
`web/manifest.json` is essential: CGB-Bit has no Improv Serial, and without it ESP Web Tools erases
the whole chip on every install (wiping saves, and leaving no firmware after "Replace the game
only"). For a player's own game the page builds a manifest as a `blob:` URL pointing at the picked
file ("Replace the game only" = the game at the `rom` offset; "Install CGB-Bit with this game" =
firmware part + game). `window.gbBit.useFile` is exposed for tests.

CI (`.github/workflows/build.yml`): tests job (smoke test + node test), firmware job in a
digest-pinned `espressif/idf:v5.4.1` container, and on `v*` tags a release job (the only job with
`contents: write`) and a GitHub Pages deploy job; the `github-pages` environment must allow `v*`
tags (README, Development). All actions are pinned to commit SHAs with the version in a comment;
Dependabot updates them and the `walnut-cgb` submodule weekly.

## Conventions

- `demo.cpp`'s input script is frame-counted for Tobu Tobu Girl Deluxe (`roms/tobudx.gb`); its last
  step (`Auto`) hands the level to `autopilot()`, which reads the girl, enemies and hazards from OAM
  by tile (`entity_sprites` in the game's `game.c`) and follows its rules: bounce only by falling
  onto an enemy, dash up (A + Up) at the top of a rise when an enemy above can catch her, boost (B)
  when nothing below is in reach. Deterministic, but the run is only checked up to frame 2490 (she
  misses a landing soon after), so changing the bundled game, the autopilot or anything affecting
  gameplay timing needs the run checked again: contact sheets of the raw frames, before
  regenerating `demo.gif`.
- GIF workflow (`tools/make_demo.sh`): `demo.cpp` writes raw 128x128 RGB565 frames (every 3rd
  Game Boy frame, 20 FPS); `make_demo_gif.py` turns them into an exact GIF at 2x (a 256-colour
  palette per frame); `gifsicle --no-warnings -O3 --colors 256` writes `demo.gif` with one shared
  palette, which lets it make unchanged pixels transparent: 2.18 MB → 1.36 MB, ~14% of pixels
  get a close colour instead (the scaler's blends), not visible side by side. Further `--lossy`
  gains under 1%. Output is deterministic (same md5 run to run). Keep `demo.gif` small (~1.5 MB)
  for the README; to judge a change, compare frames of the exact and final GIFs side by side.
- Bundled ROMs must be redistributable; `roms/README.md` records source, licences and SHA-256.
- Repository code is CC0; third-party parts keep their licences (see README "License").
