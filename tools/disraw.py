"""Disassemble SH-4 code straight out of a RAM dump (for code that is not in the
program image, e.g. what the BIOS leaves behind).

    python3 -I tools/disraw.py <ram_dump.bin> <start> [count]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh4 import decode  # noqa: E402

RAM_BASE = 0x0C000000


def main(path, start, count):
    ram = open(path, "rb").read()
    for k in range(count):
        a = start + 2 * k
        o = (a & 0x1FFFFFFF) - RAM_BASE
        op = struct.unpack_from("<H", ram, o)[0]
        i = decode(op, a)
        note = ""
        if i.lit_size == 4:
            lo = (i.lit & 0x1FFFFFFF) - RAM_BASE
            note = f"  ; =0x{struct.unpack_from('<I', ram, lo)[0]:08x}"
        print(f"{a:08x}: {op:04x}  {i.text}{note}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 64)
