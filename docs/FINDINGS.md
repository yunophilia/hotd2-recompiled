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
