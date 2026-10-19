# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow SemVer.

## [Unreleased]

The runtime is rebuilt as a generic model of the MVS board (#1). The previous runtime (v0.1.0) was shaped around Neo Drift Out with title-specific patches; nothing from it survives except the name.

### Added
- **Pause menu** in the window (Esc, or a gamepad's Guide button): volume, fullscreen, scanlines, 4:3 or 16:9 for the next start, remappable keys for both players, reset and quit, saved per game in SDL's per-user folder. Headless runs never read the settings. Text uses the public-domain font8x8 (ext/font8x8).
- **Player 2 and gamepads** in the window: player 2 on R F D G and A S Q W (MAME's layout), 2 and 4; up to two SDL game controllers, hot-plugged, the first for player 1.
- **Sound**: YM2610 synthesis (FM, SSG, ADPCM-A/B from the V ROMs) through [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3, ext/ymfm). Samples are generated up to the Z80's clock before each register write, at the chip's rate (8 MHz / 144); the window plays them through SDL2, `--wav FILE` writes them, `--mute` silences the window. The YM2610 timers and IRQ stay modelled in `audio.c`.
- **Wider output**: `--width N` (320-496) and `--widescreen` (400) add margins showing what lies just off screen; the fix layer stays centred without its overscan columns, and the sprites-per-line limit grows with the width. Screenshots and `--record` follow the width.
- **Margin fill**: `ng_game_t.extend` lets a game add copies of sprite columns to fill the margins of screens drawn for 320 pixels: mirrored, in a single colour of the sprite's palette, or showing art of the game's own (indexed pixels drawn through palette RAM, so they fade with the screen). `ng_read_indexed_png` reads such art; it uses stb_image's zlib decoder (ext/stb). The renderer draws each copy right after its original, so priority holds; VRAM and emulation are untouched. With wider output, a solid row in the fix layer's edge columns (a band, not text) carries on into the margins.
- **Game patches**: `ng_game_t.patch` changes the P ROM at load time given the output width, and `ng_game_t.live_words` lists patched immediates, which recompiled code reads from memory (with `bus_peek16`, so `--verify` stays exact).
- **Multi-bank P ROMs** (up to 8 banks of 1 MB): the recompiler analyses each bank behind `$200000` in its own context, entries for banks other than 0 carry the bank in bits 24-31, and the runtime dispatches through one table per bank, picked by the mapped bank. `--dump-misses` records the bank. KOF98 (5 MB) verifies with 0 mismatches.
- **AES mode** (`ng_game_t.aes`) and a default system ROM per game (`ng_game_t.bios`), used by both the player and the generator. On an AES the fix layer and the Z80 always use the cartridge's S and M1 ROMs.
- **m68krecomp**, the 68000 static recompiler: a full 68000 decoder; recursive-descent discovery from vectors, cartridge header entry points, static call targets, jump tables and code pointers; readable C output with address and disassembly on every line; `--entries` for runtime-found entry points; a `--raw` mode and a standalone tool for flat images.
- **Flat dispatch execution model**: return addresses on the emulated stack, interrupts as real 68000 exceptions, and a [Musashi](https://github.com/kstenerud/Musashi) fallback for addresses without a recompiled entry.
- **`--verify`**: every native block is replayed in Musashi against the same memory and I/O values, and registers, flags and writes are compared.
- **Conformance harness** (`ng_conformance`, CTest): Musashi's 60 68000 test programs, recompiled and run natively, with a reference column and a regression baseline. Currently 59/60, the same as the reference.
- **Board model**: full 68000 memory map with vector swap, P ROM banking and backup RAM lock; LSPC VRAM ports, raster counter, timer IRQ and auto-animation; per-scanline sprite renderer (sticky chains, L0 shrink, horizontal shrink, 96/line) and fix layer; palette banks; controllers, status ports, system latch and uPD4990A calendar; Z80 with banking, NMI/reply latches and YM2610 timers.
- Boot through the real MVS system ROM. There is no BIOS HLE.
- Headless mode, `--record` (ffmpeg), `--screenshot`, scripted `--input`, `--interp`, `--dump-misses`, `NG_PROFILE`, and symbolized crash reports on Windows.
- The SDL2 window is optional; headless-only builds need no dependencies.
- CI on Ubuntu (gcc) and Windows (MSVC).
- Docs: architecture, recompiler, running, conformance, porting a game, hardware notes.

### Changed
- Neo Drift Out is ported to the generic runtime with no toolkit changes. Its old per-game overrides are deleted, and it boots, attracts and races verified.
- Games describe only their ROM layout (`ng_game_t`) and hand the runtime their recompiled table through `ng_main()`.

### Removed
- The Drift Out-specific runtime: hardcoded USER/VBlank entry points, BIOS RAM guards, forced palettes and shrink, and the BIOS stub. The old public headers (`func_table.h`, `m68k.h`, ...) are gone.

### Fixed (found by the new checks)
- `MOVE.L`/`MOVEM.L` to `-(An)` store the low word first.
- `.w` absolute addresses keep their sign extension in `LEA`/`PEA`.
- `NBCD` with a zero result leaves its destination unwritten.

## [0.1.0]
- The initial runtime, brought up on Neo Drift Out.
