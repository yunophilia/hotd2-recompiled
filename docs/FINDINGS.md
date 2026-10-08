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

- The BIOS copies `ic22[0x000000:0x123000]` verbatim to **`0x0C020000–0x0C143000`**.
  Nothing else in RAM is code (checked by `rts;nop` density and by diffing dumps).
- crt0 at `0x0C020000`: zero BSS from a table at `0x0C020060`, load SP, jump to main.

| Symbol | Address |
|---|---|
| entry / crt0 | `0x0C020000` |
| crt0 table | `0x0C020060` (bss, word count, sp, main) |
| BSS | `0x0C145E10–0x0C9D8704` (≈ 8.6 MB) |
| stack top | `0x0CA553C0` |
| `main` | `0x0C088DA4` |

- `tools/funcs.py`: 1778 functions found by recursive descent from the entry, `bsr`
  targets and prologue pointers, covering ~490 KB (42 % of the image). The
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

## Not yet known

- Where the PVR2 Tile Accelerator display lists are built and submitted.
- The AICA sound driver: ARM7 code uploaded to sound RAM.
- The JVS lightgun input path.
- Which cart ROMs hold which asset types.

## Rules

Never commit ROMs, RAM dumps or disassembly listings. They are the game's
code and data. Commit only tools, notes and our own code.
