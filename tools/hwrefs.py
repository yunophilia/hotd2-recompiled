"""List which functions reference which NAOMI hardware blocks.

Scans each discovered function's PC-relative 32-bit literals for addresses in
known MMIO ranges (any P0-P4 mirror). This is the to-do list for the HLE runtime.

    python3 -I tools/hwrefs.py <hotd2.zip>
"""
import collections
import sys
import zipfile

import funcs
from sh4 import decode

# (name, start, end) on the 29-bit physical bus
BLOCKS = [
    ("holly-sb", 0x005F6800, 0x005F7000),   # system bus: DMA, interrupts
    ("maple", 0x005F6C00, 0x005F6D00),
    ("g1-cart", 0x005F7400, 0x005F7500),
    ("g2", 0x005F7800, 0x005F7900),
    ("pvr-regs", 0x005F8000, 0x005FA000),
    ("naomi-cart", 0x005F7000, 0x005F7100),  # ROM PIO/DMA regs
    ("aica-regs", 0x00700000, 0x00710000),
    ("aica-rtc", 0x00710000, 0x00710010),
    ("aica-ram", 0x00800000, 0x01000000),
    ("vram64", 0x04000000, 0x05000000),
    ("vram32", 0x05000000, 0x06000000),
    ("ta-fifo", 0x10000000, 0x12000000),
    ("ta-yuv/tex", 0x12000000, 0x14000000),
    ("sh4-onchip", 0x1F000000, 0x20000000),
    ("store-queue", 0xE0000000, 0xE4000000),
]


def classify(v):
    if 0xE0000000 <= v < 0xE4000000:
        return "store-queue"
    if v >= 0xFF000000:
        return "sh4-onchip"
    phys = v & 0x1FFFFFFF
    for name, lo, hi in BLOCKS:
        if lo <= phys < hi and name not in ("store-queue",):
            return name
    return None


def main(zip_path):
    data = zipfile.ZipFile(zip_path).read("epr-21585.ic22")[:funcs.PROGRAM_SIZE]
    img = funcs.Image(data)
    fs = funcs.discover(img)
    by_block = collections.defaultdict(set)
    for f, (body, _) in fs.items():
        for pc in body:
            i = decode(img.u16(pc), pc)
            if i.lit_size == 4 and img.has(i.lit, 4):
                v = img.u32(i.lit)
                b = classify(v)
                # ignore small constants that only look like low physical addresses
                if b and (v >= 0x005F0000):
                    by_block[b].add(f)
    for b, fns in sorted(by_block.items(), key=lambda kv: -len(kv[1])):
        lst = " ".join(f"{x:08x}" for x in sorted(fns)[:12])
        print(f"{b:12} {len(fns):4} funcs  {lst}{' ...' if len(fns) > 12 else ''}")


if __name__ == "__main__":
    main(sys.argv[1])
