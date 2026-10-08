"""Compare words at given addresses across RAM dumps (harness vs Flycast).

    python3 -I tools/ramcmp.py <addr>[:count] dump1.bin dump2.bin ...
"""
import struct
import sys

BASE = 0x0C000000


def main():
    spec, dumps = sys.argv[1], sys.argv[2:]
    addr, _, count = spec.partition(":")
    addr, count = int(addr, 16), int(count or "1", 0)
    datas = [open(d, "rb").read() for d in dumps]
    print("address   " + "  ".join(f"{d.split('/')[-1]:>12}" for d in dumps))
    for k in range(count):
        a = addr + 4 * k
        o = (a & 0x1FFFFFFF) - BASE
        vals = [struct.unpack_from("<I", d, o)[0] for d in datas]
        mark = "" if len(set(vals)) == 1 else "  <-- differs"
        print(f"{a:08x}  " + "  ".join(f"{v:12x}" for v in vals) + mark)


if __name__ == "__main__":
    main()
