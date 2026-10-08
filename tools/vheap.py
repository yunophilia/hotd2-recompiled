"""Dump the SDK's VRAM heaps (allocator f_0c0da1c0) from a RAM dump.

Heap descriptors are 20 bytes each starting at 0x0C145D70; +8 is the head of
a linked list whose nodes hold flags at +0, next at +8, address at +12 and
size at +16.

    python3 -I tools/vheap.py <ram_dump.bin> [heaps]
"""
import struct
import sys

BASE = 0x0C000000


def w(ram, a):
    o = (a & 0x1FFFFFFF) - BASE
    if not 0 <= o < len(ram) - 3:
        return None
    return struct.unpack_from("<I", ram, o)[0]


def main(path, heaps=4):
    ram = open(path, "rb").read()
    for h in range(heaps):
        d = 0x0C145D70 + 20 * h
        fields = [w(ram, d + 4 * k) for k in range(5)]
        print(f"heap {h} @ {d:08x}: " + " ".join(f"{x:08x}" for x in fields))
        for off, name in ((4, "list@+4"), (8, "list@+8")):
            node, seen = fields[off // 4], set()
            while node and node not in seen and len(seen) < 32:
                seen.add(node)
                flags = w(ram, node) or 0
                print(f"    {name} node {node:08x}: flags={flags & 0xFFFF:04x} addr={w(ram, node + 12):08x} size={w(ram, node + 16):08x}")
                node = w(ram, node + 8)


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 4)
