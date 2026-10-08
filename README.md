# hotd2-web

A browser (WebAssembly) port of *The House of the Dead 2* (Sega NAOMI, 1998),
built by statically recompiling the original SH-4 program and decompiling it
function by function. Bring your own dump: this repo contains no game code or data.

## Status

- [x] Phase 1: cart layout, program load address, crt0/main, function discovery (`docs/FINDINGS.md`)
- [~] Phase 0: Flycast compiles and links to WASM (`patches/flycast/0002-*`) and `web/` loads it.
  It stops after renderer init and never boots the game. Parked: native Flycast in WSL is the reference for now.
- [~] Phase 2: the recompiler (`tools/recomp.py`) turns all 2242 functions into C that builds and runs natively.
  The game boots to Maple/JVS init. Next: JVS HLE, then PVR2 TA → WebGL2, then AICA.
- [ ] Phase 3: step-by-step decompilation
- [ ] Phase 4: web shell (ROM picker, IndexedDB, input, saves)

## Layout

| Path | What |
|---|---|
| `tools/sh4.py` | SH-4 instruction decoder |
| `tools/funcs.py` | function discovery and disassembly of the program image |
| `tools/ramscan.py` | compares a RAM dump with the program ROM |
| `tools/recomp.py` | SH-4 → C recompiler (writes `out/gen/`, never committed) |
| `tools/hwrefs.py` | which functions touch which hardware block |
| `tools/cart.py` | builds a flat cart image from the ROM set |
| `patches/flycast/` | patches for the reference emulator (RAM-dump hook, Emscripten build) |
| `runtime/` | C runtime for recompiled code (guest state, memory, FPU helpers) |
| `web/` | browser shell + dev server with COOP/COEP headers (`python web/serve.py`) |
| `docs/FINDINGS.md` | what is known about the hardware and program so far |

## Reference emulator (WSL)

Flycast is cloned into `~/hotd2/flycast` at `patches/flycast/BASE_COMMIT`, with the
patches applied and built with `cmake -G Ninja -DUSE_VULKAN=OFF -DUSE_BREAKPAD=OFF -DUSE_LUA=OFF`.
`hod2bios.zip` must be in the same folder as `hotd2.zip`.

```bash
HOTD2_DUMP_DIR=~/hotd2/dumps HOTD2_DUMP_FRAMES=300,1200,3000 ./build-native/flycast ~/hotd2/roms/hotd2.zip
```

## Recompiled build (WSL)

```bash
python3 -I tools/recomp.py ~/hotd2/roms/hotd2.zip out/gen
gcc -O1 -w -Iruntime out/gen/*.c runtime/harness.c -lm -o harness
./harness ic22.bin            # ic22.bin = epr-21585.ic22 from your dump
```
