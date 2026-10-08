"""Summarise what the BIOS leaves in 0x0C000000-0x0C020000 (from a Flycast RAM dump).

    python3 -I tools/biosarea.py ~/hotd2/dumps/ram_300.bin
"""
import struct
import sys

BASE = 0x0C000000


def main(path):
    ram = open(path, "rb").read()
    low = ram[:0x20000]
    # non-zero 256-byte blocks
    blocks = [o for o in range(0, len(low), 0x100) if any(low[o:o + 0x100])]
    runs, start = [], None
    for o in range(0, len(low) + 0x100, 0x100):
        on = o in blocks
        if on and start is None:
            start = o
        if not on and start is not None:
            runs.append((start, o))
            start = None
    print("non-zero regions:")
    for a, b in runs:
        rts = low[a:b].count(bytes.fromhex("0b000900"))
        print(f"  {BASE + a:08x}-{BASE + b:08x}  {b - a:6d} bytes  rts;nop={rts}")
    # words in low RAM that point into the game program or low RAM itself
    print("pointer-like words in 0x0C000000-0x0C000400:")
    for o in range(0, 0x400, 4):
        v = struct.unpack_from("<I", low, o)[0]
        if 0x0C000000 <= (v & 0x1FFFFFFF) < 0x0D000000:
            print(f"  {BASE + o:08x}: {v:08x}")


if __name__ == "__main__":
    main(sys.argv[1])
