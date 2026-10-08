"""Show the keyed-on AICA channels in two register dumps side by side.

    python3 -I tools/aicacmp.py ours_aregs.bin flycast_aregs.bin
"""
import struct
import sys

FIELDS = [
    ("KYONB", 0x00, 14, 1), ("LPCTL", 0x00, 9, 1), ("PCMS", 0x00, 7, 3),
    ("SA", None, 0, 0), ("LSA", 0x08, 0, 0xFFFF), ("LEA", 0x0C, 0, 0xFFFF),
    ("AR", 0x10, 0, 31), ("D1R", 0x10, 6, 31), ("D2R", 0x10, 11, 31),
    ("RR", 0x14, 0, 31), ("DL", 0x14, 5, 31), ("KRS", 0x14, 10, 15),
    ("OCT", 0x18, 11, 15), ("FNS", 0x18, 0, 0x3FF),
    ("IMXL", 0x20, 4, 15), ("ISEL", 0x20, 0, 15),
    ("DISDL", 0x24, 8, 15), ("DIPAN", 0x24, 0, 31),
    ("TL", 0x28, 8, 255), ("VOFF", 0x28, 6, 1),
]


def r16(d, off):
    return struct.unpack_from("<H", d, off)[0]


def chan(d, c):
    b = c * 0x80
    out = {}
    for name, off, sh, mask in FIELDS:
        if name == "SA":
            out[name] = ((r16(d, b) & 0x7F) << 16) | r16(d, b + 4)
        else:
            out[name] = (r16(d, b + off) >> sh) & mask
    return out


def main():
    a, b = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
    print("common: ours MVOL=%x 2800=%04x | flycast MVOL=%x 2800=%04x" % (
        r16(a, 0x2800) & 15, r16(a, 0x2800), r16(b, 0x2800) & 15, r16(b, 0x2800)))
    for c in range(64):
        ca, cb = chan(a, c), chan(b, c)
        if not (ca["KYONB"] or cb["KYONB"]):
            continue
        diff = [k for k in ca if ca[k] != cb[k]]
        print(f"ch{c:2d} ours   " + " ".join(f"{k}={ca[k]:x}" for k in ca))
        print(f"     flycast " + " ".join(f"{k}={cb[k]:x}" for k in cb) + (f"   DIFF: {diff}" if diff else ""))


if __name__ == "__main__":
    main()
