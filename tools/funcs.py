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


def _writes(text, reg):
    """True if the instruction text writes register `reg` (roughly: it is the last operand)."""
    return text.endswith("," + reg) or text.endswith(" " + reg) and text.split()[0] in (
        "movt", "dt", "shll", "shlr", "shll2", "shlr2", "shll8", "shlr8", "shll16", "shlr16",
        "shal", "shar", "rotl", "rotr", "rotcl", "rotcr")


def reg_literal(img, pc, reg, lookback=16):
    """Value of a `mov.l @(lit),reg` reaching pc in straight-line code, else None."""
    a = pc - 2
    while a >= pc - 2 * lookback and img.has(a):
        i = decode(img.u16(a), a)
        if i.jump or i.cond:
            return None
        if _writes(i.text, reg):
            if i.lit_size == 4 and img.has(i.lit, 4):
                return img.u32(i.lit)
            return None
        a -= 2
    return None


def switch_targets(img, braf_pc, max_cases=256):
    """Targets of an SHC `mova tbl,r0; mov.w @(r0,rX),r0; braf r0` switch."""
    mova = None
    for a in range(braf_pc - 2, braf_pc - 12, -2):
        i = decode(img.u16(a), a)
        if i.text.startswith("mova"):
            mova = i.lit
            break
    if mova is None:
        return []
    # bound: the nearest "cmp/hi rB,r0" with "mov #N,rB" before it
    bound = None
    for a in range(braf_pc - 2, braf_pc - 40, -2):
        if not img.has(a):
            break
        i = decode(img.u16(a), a)
        if i.text.startswith("cmp/hi") and i.text.endswith(",r0"):
            rb = i.text.split()[1].split(",")[0]
            for b in range(a - 2, a - 20, -2):
                j = decode(img.u16(b), b)
                if j.text.startswith("mov #") and j.text.endswith("," + rb):
                    bound = int(j.text.split("#")[1].split(",")[0])
                    break
            break
    n = bound + 1 if bound is not None and 0 <= bound < max_cases else 0
    out = []
    for k in range(n):
        if not img.has(mova + 2 * k):
            break
        off = img.u16(mova + 2 * k)
        off = off - 0x10000 if off & 0x8000 else off
        out.append(braf_pc + 4 + off)
    return out


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
            if i.indirect and i.text.startswith(("jsr", "jmp")):  # jmp = tail call
                t = reg_literal(img, pc, i.text.split("@")[1])
                if t is not None and img.has(t) and t % 2 == 0:
                    calls.add(t)
            if i.text.startswith("braf"):
                todo.extend(t for t in switch_targets(img, pc) if img.has(t))
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
