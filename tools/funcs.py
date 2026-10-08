"""Recursive-descent function discovery + disassembly for the HOTD2 program.

Seeds: the image start (entry), every bsr target, and every 32-bit literal that
points at a plausible prologue inside the code range. Each function is walked
until rts/jmp/bra-out, following conditional and in-function branches.

    python3 -I tools/funcs.py <hotd2.zip> --list            # function table
    python3 -I tools/funcs.py <hotd2.zip> --dis 0x0c020000  # disassemble one
"""
import argparse
import struct
import zipfile

from sh4 import decode

LOAD = 0x0C020000
PROGRAM_SIZE = 0x123000          # ic22 bytes the BIOS copies to RAM
PROLOGUES = {0x2FE6, 0x4F22, 0x2FD6, 0x2FC6, 0x2FB6, 0x2FA6, 0x2F96, 0x2F86}


class Image:
    def __init__(self, data):
        self.data = data
        self.lo, self.hi = LOAD, LOAD + len(data)

    def has(self, addr, size=2):
        return self.lo <= addr and addr + size <= self.hi

    def u16(self, addr):
        return struct.unpack_from("<H", self.data, addr - self.lo)[0]

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.lo)[0]


def walk(img, entry):
    """Return (blocks, calls, literals) for the function starting at entry."""
    seen, todo, calls, lits = set(), [entry], set(), set()
    while todo:
        pc = todo.pop()
        while img.has(pc) and pc not in seen:
            seen.add(pc)
            i = decode(img.u16(pc), pc)
            if not i.valid:
                break
            if i.lit is not None and i.lit_size:
                lits.add(i.lit)
            if i.call and i.target is not None:
                calls.add(i.target)
            if i.cond and i.target is not None:
                todo.append(i.target)
            if i.delay:
                seen.add(pc + 2)
                d = decode(img.u16(pc + 2), pc + 2) if img.has(pc + 2) else None
                if d and d.lit is not None and d.lit_size:
                    lits.add(d.lit)
            if i.jump:
                if i.target is not None and not i.call:
                    todo.append(i.target)  # bra: treat as intra-function
                break
            pc += 4 if i.delay else 2
    return seen, calls, lits


def discover(img):
    funcs, todo = {}, [img.lo]
    # literal pointers to prologues anywhere in the image
    for off in range(0, len(img.data) - 3, 4):
        v = struct.unpack_from("<I", img.data, off)[0]
        if img.has(v) and v % 2 == 0 and img.u16(v) in PROLOGUES:
            todo.append(v)
    while todo:
        f = todo.pop()
        if f in funcs or not img.has(f):
            continue
        body, calls, lits = walk(img, f)
        funcs[f] = (body, calls)
        todo.extend(calls)
        for a in lits:
            if img.has(a, 4):
                v = img.u32(a)
                if img.has(v) and v % 2 == 0 and img.u16(v) in PROLOGUES:
                    todo.append(v)
    return funcs


def disassemble(img, f, funcs):
    body = sorted(funcs[f][0]) if f in funcs else []
    for pc in body:
        i = decode(img.u16(pc), pc)
        note = ""
        if i.lit is not None and i.lit_size == 4 and img.has(i.lit, 4):
            v = img.u32(i.lit)
            note = f"  ; =0x{v:08x}" + (" <func>" if v in funcs else "")
        mark = "<" if pc in funcs else " "
        print(f"{mark}{pc:08x}: {i.op:04x}  {i.text}{note}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dis", type=lambda s: int(s, 0))
    a = ap.parse_args()
    data = zipfile.ZipFile(a.zip).read("epr-21585.ic22")[:PROGRAM_SIZE]
    img = Image(data)
    funcs = discover(img)
    covered = sum(len(b) for b, _ in funcs.values()) * 2
    print(f"{len(funcs)} functions, {covered / 1024:.0f} KB of code reached "
          f"({100 * covered / len(data):.1f}% of image)")
    if a.list:
        for f in sorted(funcs):
            body, calls = funcs[f]
            print(f"{f:08x} size~{len(body) * 2:6d} calls={len(calls)}")
    if a.dis is not None:
        disassemble(img, a.dis, funcs)


if __name__ == "__main__":
    main()
