"""Walk the game's task list in a RAM dump.

The scheduler (0x0C0B8264) keeps the current task pointer at 0x0C9C2C3C;
each task has its function at +0 and the next task at +24.

    python3 -I tools/tasks.py <ram_dump.bin> [head_ptr_addr]
"""
import struct
import sys

BASE = 0x0C000000


def word(ram, a):
    o = (a & 0x1FFFFFFF) - BASE
    if not 0 <= o < len(ram) - 3:
        return None
    return struct.unpack_from("<I", ram, o)[0]


def main(path, head_addr=0x0C9C2C3C):
    ram = open(path, "rb").read()
    t = word(ram, head_addr)
    print(f"{path}: current task = {t:08x}")
    seen = set()
    for _ in range(64):
        if t is None or t in seen or (t & 0x1FFFFFFF) < BASE:
            print(f"  stop at {t}")
            break
        seen.add(t)
        fn, nxt = word(ram, t), word(ram, t + 24)
        print(f"  task {t:08x}: fn={fn:08x} next={nxt:08x}")
        t = nxt


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x0C9C2C3C)
