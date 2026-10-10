# hotd2-recompiled

A browser (WebAssembly) port of *The House of the Dead 2* (Sega NAOMI, 1998),
built by statically recompiling the original SH-4 program to C and replacing the
NAOMI hardware with high-level emulation. Bring your own dump: this repository
contains no game code or data, only tools and our own runtime.

## Status

- [x] **Program analysis**: cart layout, program image (`ic22[0:0x125E10]` at
  `0x0C020000`), crt0/main, ~2,700 functions found by recursive descent, literal
  and data-table scans, plus run-time seeds (`data/seeds.txt`).
- [x] **Recompiler** (`tools/recomp.py`): every function becomes C; delay slots,
  switch tables, coroutine save/restore, cycle counting for timing.
- [x] **SH-4 interpreter fallback** (`runtime/interp.c`) for code the static
  analysis never saw; the browser build keeps running instead of aborting.
- [x] **Hardware HLE** (`runtime/hle.c` and friends): BIOS interrupt dispatch,
  Holly interrupts and video timing, Maple/MIE/JVS I/O board (with the game's
  uploaded MIE firmware mode), EEPROM/SRAM, cart ROM PIO and DMA, TMU, store
  queues, channel-2 DMA, TA list parsing, BIOS region (default USA).
- [x] **Graphics**: PVR2 display-list and texture decoder (`runtime/pvr.c`) with
  a reference software rasteriser, and a **WebGL2 renderer** in the browser.
- [x] **Sound**: ARM7 interpreter + AICA model run the game's own sound driver;
  output via AudioWorklet in the browser, with sample interpolation. (HOTD2 leaves
  the per-voice filter and effects DSP unused, so they are not emulated.)
- [x] **Browser build**: 60 fps through all chapters tested so far (automated
  play reaches chapter 3), mouse/touch gun, full-resolution WebGL rendering with
  fog and render-to-texture, fullscreen, NAOMI test menu (`web/play/`).
- [x] **Saves**: EEPROM and SRAM persist in the browser (localStorage).
- [x] **Recompiler cross-check**: per-function comparison against the interpreter
  (`runtime/diff.c`), 972 functions checked over 30,000 frames, no differences.
- [ ] Coverage of the last chapters and the ending (in progress).

## Playing

Load your `hotd2.zip`, then: **5** inserts coins (one game's worth), **1** or
**Enter** starts, the mouse is the gun (left button fires, right button aims
off-screen to reload), **F** toggles fullscreen. **9** opens the NAOMI test menu,
**0** is the service button that moves its cursor, **9** again selects.

See `docs/FINDINGS.md` for everything learned about the hardware and the game.

## Layout

| Path | What |
|---|---|
| `tools/recomp.py` | SH-4 → C recompiler (writes generated C, never committed) |
| `tools/sh4.py`, `tools/funcs.py` | SH-4 decoder, function discovery and disassembly |
| `tools/*.py` | analysis helpers: RAM/state diffs against Flycast, task list, VRAM heaps, ARM disassembler, audio and AICA comparisons, PPM → PNG |
| `runtime/` | C runtime: guest state, HLE hardware, JVS, cart, TA, PVR renderer, AICA + ARM7, coroutines, interpreter, native harness and browser entry |
| `runtime/build-harness.sh` | native test build (WSL) |
| `runtime/build-web.sh` | browser build (Emscripten, WSL) → `web/game/` |
| `runtime/seed-play.sh` | scripted play sessions that collect code the static analysis missed |
| `web/play/` | browser front end: zip loader, WebGL2 renderer, audio worklet, input |
| `web/serve.py` | dev server with the COOP/COEP headers threads need |
| `patches/flycast/` | patches for the reference emulator (RAM/VRAM/sound dumps, PCM dump, Emscripten build) |
| `data/seeds.txt` | entry points found at run time |
| `docs/FINDINGS.md` | the findings log |

## Building (WSL)

```bash
runtime/build-harness.sh ~/hotd2/roms/hotd2.zip ~/hotd2/hb     # native harness
runtime/build-web.sh ~/hotd2/roms/hotd2.zip                    # browser build into web/game/
python web/serve.py 8642                                       # then open http://localhost:8642/play/
```

The browser page asks for your `hotd2.zip`; no BIOS is needed. On localhost the
dev server can also serve it from `HOTD2_ROM_DIR` (default `C:\RetroBat\roms\naomi`).

### Harness options

`HOTD2_TRACE=0` (fast, no tracing) · `HOTD2_FRAMES=n` · `HOTD2_INTERP=1`
(interpret unknown code instead of stopping) · `HOTD2_INPUT="1300:coin,1330:start,…"` ·
`HOTD2_SHOTS=render numbers` (software-rendered frames as PPM) · `HOTD2_WAV=out.wav` ·
`HOTD2_DUMP_FRAMES=…` (RAM and AICA register dumps) · `HOTD2_REGION=0|1|2` ·
`HOTD2_LISTCRC=file` / `HOTD2_LISTDUMP=renders` (display-list CRCs / raw lists) ·
`HOTD2_INTERP_ALL=1` (interpret everything) · `HOTD2_FREEZE=addr=byte@frame,…`
(hold RAM bytes, e.g. infinite lives for coverage) · `HOTD2_INPUT=@file` (script from
a file). Build with `HOTD2_CFLAGS=-DHOTD2_DIFF` for the per-function
recompiler-vs-interpreter check (`runtime/diff.c`).

`HOTD2_LISTDUMP=N` also writes VRAM and PVR register dumps; copy them to `web/replay/`
(gitignored) and open `play/?replay=N[,M,…]` to draw those frames with the WebGL
renderer. `play/?mute` runs the audio player at zero volume (`hotd2AudioStats` in
the console reports buffer health).

## Reference emulator

Flycast at `patches/flycast/BASE_COMMIT` with the patches applied, built natively in
WSL, is used only as a reference: RAM, VRAM, sound RAM and AICA register dumps at
chosen frames (`HOTD2_DUMP_DIR`, `HOTD2_DUMP_FRAMES`) and a raw PCM dump of its
audio (`HOTD2_PCM`).
