# HOTD2 (NAOMI) — reverse-engineering findings

Dump: `hotd2.zip` (MAME/Flycast set "hotd2", USA) + `hod2bios.zip`.
Reference emulator: Flycast `0d9853d` (see `patches/flycast/BASE_COMMIT`).

## Cartridge

- Board type **M2**, cart size `0xA800000`. No 315-5881 key: data is **not encrypted**.
- Layout (`tools/cart.py`): `ic22` (2 MB) at `0x0000000` and reloaded at `0x0200000`;
  `ic1..ic20s` (8 MB each) at `0x0800000 * n`.
- M2 boots in "4MB mode": CPU offset → ROM offset is
  `(o & 0x103FFFFF) | ((o & 0x07C00000) << 1)`.
- There is **no standard `NAOMI` boot header** in ic22. It boots through the
  dedicated `hod2bios` (epr-21329/30/31 for JP/US/EX), which must sit beside the ROM.

## Program image

- The BIOS copies `ic22[0x000000:0x125E10]` (code + initialised .data) verbatim to **`0x0C020000–0x0C145E10`**, right up to BSS.
  Nothing else in RAM is code (checked by `rts;nop` density and by diffing dumps).
- crt0 at `0x0C020000`: zero BSS from a table at `0x0C020060`, load SP, jump to main.

| Symbol | Address |
|---|---|
| entry / crt0 | `0x0C020000` |
| crt0 table | `0x0C020060` (bss, word count, sp, main) |
| BSS | `0x0C145E10–0x0C9D8704` (≈ 8.6 MB) |
| stack top | `0x0CA553C0` |
| `main` | `0x0C088DA4` |

- `tools/funcs.py`: 2242 functions found by recursive descent from the entry, `bsr`
  targets, `mov.l lit; jsr/jmp @rN` call sites, SHC `braf` switch tables and
  prologue pointers, covering ~536 KB (46 % of the image). The
  rest is read-only data plus code reached only through jump tables and
  function-pointer tables still to be resolved.

## RAM behaviour (attract mode, frames 300/1200/3000)

Approximate, at 64 KB granularity:

- Identical in all three dumps: `0x0C010000–0x0C140000` (the program) and
  `0x0C690000–0x0C940000`.
- Unchanged from frame 1200 onwards (probably assets loaded after boot):
  `0x0C1D0000–0x0C350000` and roughly `0x0CCC0000–0x0D0C0000`.
- Still changing: most of `0x0D0C0000–0x0D980000` (heap and working buffers).
- Above `0x0D980000`: unused.

## Hardware access (`tools/hwrefs.py`)

Counted from each function's 32-bit PC-relative literals, so these are lower bounds:

| Block | Functions | Notes |
|---|---|---|
| SH-4 on-chip regs | 134 | timers, DMAC, cache control, CCN |
| VRAM (64-bit path) | 65 | some may be float constants that happen to look like addresses |
| TA FIFO | 55 | display-list writers, spread across game code |
| AICA RAM | 35 | sound driver upload and sound commands |
| Holly system bus | 11 | all in `0x0C0B2630–0x0C0DBDC2` |
| store queues | 9 | `0x0C0B4920–0x0C0D324A`, fast TA/texture transfer |
| PVR regs | 8 | mostly `0x0C0D7D10–0x0C0D89BA` |
| G1 / NAOMI cart | 6 / 2 | `0x0C0B2180–0x0C0C0228`, ROM PIO/DMA reads |

So the low-level hardware layer is concentrated in **`0x0C0B0000–0x0C0E0000`**,
which looks like the Sega SDK libraries (Kamui/Ninja-style). That is the obvious
seam for HLE: replace those functions wholesale and leave the game code
above them recompiled.

## Recompiled boot (`tools/recomp.py` + `runtime/harness.c`)

All 2242 functions recompile to C and build natively, with no unhandled opcodes.
Started at crt0 with only the program image in RAM, the game runs through:

1. BSC refresh setup: `FFA00040` (RFCR), `FFA00020`, `FFA0002C`
2. G1 bus timing: `A05F7480–A05F74B8`
3. Maple DMA: command list at `0x0C9D1080`, start `A05F6C04`, enable `A05F6C14`,
   start `A05F6C18`, settings `A05F6C80` / `A05F6C8C`. It then polls `A05F6C18`
   and keeps resending, waiting for a valid JVS reply.

In steady state the list is one frame: Maple command `0x86` (JVS over Maple),
sub-command `0x15` ("receive JVS data"), reply buffer at `0x0C9D0880`. HOTD2 is
the special case Flycast calls `hotd2p`: it answers `0x15` and also sends the
repeated request. **Next HLE subsystem: a JVS I/O board** (reset, set address,
ID, feature query, switches, coins, analog/screen position for the guns).

## Not yet known

- Where the PVR2 Tile Accelerator display lists are built and submitted.
- The AICA sound driver: ARM7 code uploaded to sound RAM.
- The JVS lightgun input path.
- Which cart ROMs hold which asset types.

## Rules

Never commit ROMs, RAM dumps or disassembly listings. They are the game's
code and data. Commit only tools, notes and our own code.

## BIOS and hardware HLE (runtime/hle.c, jvs.c, cart.c, ta.c)

- The BIOS leaves VBR = `0x8C000000`. Games register interrupt callbacks at
  `VBR + 0x1C0 + (INTEVT >> 3)`; the level-6 slot is `0x8C000224`. The HLE calls
  them like ordinary functions on a private stack.
- PVR chip ID `0x17FD11DB`, revision `0x11`. With ID 0 the SDK traps forever.
- Implemented so far: Maple/MIE/JVS I/O board, EEPROM, SRAM, VRAM (32/64-bit paths),
  Holly interrupts + scanline timing, TMU, store queues, ch2 DMA to the TA (recorded,
  not yet rendered), cart PIO/G1 DMA.
- Current state: the harness runs past asset loading and into a steady loop
  (4000+ registers touched, 200k-access budget used up). Not yet compared against Flycast.

## Session 2026-10-08

Recompiler/runtime bugs fixed:
- A function whose body reaches code *below* its entry (backward `bra` into
  shared code) started executing at the lowest address. Now jumps to the entry.
  This was behind all the writes through "null" pointers at boot.
- `is_ram` covered 16 MB; NAOMI main RAM is 32 MB (`0x0C000000–0x0DFFFFFF`).

HLE added:
- hblank interrupt follows `SPG_HBLANK_INT` (mode 0/1/2) instead of every line.
- TA parameter walker: list types, 64-byte parameters, end-of-list interrupts
  (bits 7/8/9/10/21). `STARTRENDER` raises render-done (bits 0–2), not drawn yet.
- AICA sound RAM (8 MB at `0x00800000`) and register storage. **Stub:** releasing
  the ARM7 reset writes 1 to sound RAM `0x5C`, the "driver alive" word the game
  polls (`f_0c0ba70c`). Real fix = ARM7/AICA emulation or a driver HLE.

Current blocker: JVS enumeration. Flycast's RAM shows the I/O board table at
`0x0C9C3550` (handler `0x0C0BE3A4` + board ID string) with count `1` at
`0x0C9C8800`. Ours stays empty, the game later calls through the empty slot.
The game only sends MIE sub-command `0x0B` in a retry cycle, so its meaning
for this firmware is being captured from Flycast (`DUMP_JVS`).

### Later on 2026-10-08

Fixed / added:
- **Maple reply bug**: replies were written byte-by-byte to the same address
  (missing `+ k`), so every reply collapsed to its last byte. Fixed.
- MIE: `0x82` get-ID (reply code `0x83`, ID string the game compares byte for
  byte against `.data` at `0x0C143FA0`), `0x80` Z80 firmware upload (per-chunk
  8-bit checksum ack). After the upload the bridge runs HOTD2's firmware mode:
  sub-commands `0x13`/`0x17` swap, replies framed `(status, len, data)`, sense `0x8E`.
- JVS board: analog inputs (8 × 16-bit) instead of screen position, guns on
  channels 0–3 scaled from 640×480, idle channels `0x8000`; packet checksum.
  Byte-identical to Flycast's replies in a real capture (`DUMP_JVS` build).
- Sound command ring at sound RAM `0x400–0x4FF` (64 slots; driver zeroes a slot
  to consume it). Stub consumes and records commands.
- **Coroutines**: HOTD2 switches tasks with save-context `0x0C0BA0C0` /
  restore-context `0x0C0BA126` (setjmp/longjmp style, full register file). Calls
  to save are wrapped in a host `setjmp` (`CORO_SAVE`), restore goes to
  `coro_restore()` which switches host fibers (ucontext natively; the browser
  build will need Emscripten fibers). Not exercised yet: no switches so far.
- Discovery: code literals and data-section words that point into code are
  function pointers (task handlers, state tables): 2337 → 2723 functions.

Current state: boot runs to frame ~864 (822 rendered frames, 1644 TA lists,
170 firmware chunks, 3800 cart DMAs), then the main loop keeps running but
rendering stops. Flycast passes the same point (display-list sequence `0x336`,
around its frame 1210) without pausing. Compared by game progress, Flycast
then fills a table at `0x0C99D15C` (60-byte entries, base `0x0C993FE0`,
code `f_0c09a374`) that stays zero for us. `f_0c0b8264` (run-task, 16 callers)
is never reached in our run.

Tools: `tools/statediff.py` (state diff aligned by progress), `tools/tasks.py`,
`tools/ramcmp.py`, `tools/disraw.py`, `tools/biosarea.py`; harness prints
register histogram, IRQ state, TA parser state, fibers and sound commands at
stop, and dumps RAM at chosen frames (`HOTD2_DUMP_FRAMES`).

### Attract-mode stall (investigation state)

- Main loop: `main` calls per-frame work, then dispatches on the game mode at
  `0x0C3D07B4` (0–10). Mode 9 handler `f_0c020070` has sub-state `0x0C3D07B8`:
  0 = set up and create the task list (head stored at `0x0C145E10`), 1 = run-task
  (`f_0c0b8264`) every frame. Task lists match Flycast exactly.
- Scene sequencer task `f_0c06761a` runs an attract **script** loaded from the
  cart to `0x0CAE0000` (pointer `0x0C3D125C`, step count `0x0C3D1250`), dispatching
  commands through the table at `0x0C13C020`. Command `0x41` (`f_0c06821e`) =
  "wait N frames", counter at `0x0C3CFE04` advanced by `f_0c02862c`.
- Frame pipeline: 4 slots at `0x0CA25F94` with states 0 begin → 2 lists sent →
  3 ready → 4 rendering → 5 done → 7 free; in-flight flag `0x0CA25FBC`. Render
  is started from the level-6 interrupt (`f_0c0d8360` → `f_0c0d80ea`,
  STARTRENDER) when a slot is in state 3.
- Ours: everything runs normally until frame ~1083 (render 822, script step 2,
  wait counter 11 of 495), then the game logic stops beginning frames while the
  main loop and interrupts keep running. Nothing external is pending at that
  point (no cart DMA, no sound-RAM polling, TA idle). Flycast passes the same
  point without pausing (around its frame 1210).
- Ruled out so far: JVS replies (byte-identical), sound command ring, TA list
  end interrupts, TA_LIST_INIT/TA_ITP_CURRENT, cart DMA addressing.
