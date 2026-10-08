"""SH-4 instruction decoder shared by the disassembler, analyser and recompiler.

decode(op, pc) -> Insn with a textual form plus the control-flow facts the
analyser needs (branch target, delay slot, call/return, PC-relative literal).
Encodings follow the Renesas SH-4 software manual; "n"/"m" are bits 11-8 / 7-4.
"""
from dataclasses import dataclass
from typing import Optional


@dataclass
class Insn:
    pc: int
    op: int
    text: str
    target: Optional[int] = None   # static branch/call target
    delay: bool = False            # has a delay slot
    call: bool = False             # bsr/bsrf/jsr
    ret: bool = False              # rts/rte
    jump: bool = False             # unconditional transfer (bra/braf/jmp/rts/rte)
    cond: bool = False             # bt/bf(/s)
    indirect: bool = False         # target in a register
    lit: Optional[int] = None      # address of a PC-relative literal
    lit_size: int = 0
    valid: bool = True


def s8(x):
    return x - 0x100 if x & 0x80 else x


def s12(x):
    return x - 0x1000 if x & 0x800 else x


_ALU2 = {  # (top nibble, low nibble) -> mnemonic for "op Rm,Rn"
    (0x2, 0x7): "div0s", (0x2, 0x8): "tst", (0x2, 0x9): "and", (0x2, 0xA): "xor",
    (0x2, 0xB): "or", (0x2, 0xC): "cmp/str", (0x2, 0xD): "xtrct", (0x2, 0xE): "mulu.w",
    (0x2, 0xF): "muls.w",
    (0x3, 0x0): "cmp/eq", (0x3, 0x2): "cmp/hs", (0x3, 0x3): "cmp/ge", (0x3, 0x4): "div1",
    (0x3, 0x5): "dmulu.l", (0x3, 0x6): "cmp/hi", (0x3, 0x7): "cmp/gt", (0x3, 0x8): "sub",
    (0x3, 0xA): "subc", (0x3, 0xB): "subv", (0x3, 0xC): "add", (0x3, 0xD): "dmuls.l",
    (0x3, 0xE): "addc", (0x3, 0xF): "addv",
    (0x6, 0x3): "mov", (0x6, 0x7): "not", (0x6, 0x8): "swap.b", (0x6, 0x9): "swap.w",
    (0x6, 0xA): "negc", (0x6, 0xB): "neg", (0x6, 0xC): "extu.b", (0x6, 0xD): "extu.w",
    (0x6, 0xE): "exts.b", (0x6, 0xF): "exts.w",
    (0x4, 0xC): "shad", (0x4, 0xD): "shld", (0x0, 0x7): "mul.l",
}

_SZ = {0: "b", 1: "w", 2: "l"}

_SYS0 = {0x0008: "clrt", 0x0018: "sett", 0x0028: "clrmac", 0x0038: "ldtlb",
         0x0048: "clrs", 0x0058: "sets", 0x0009: "nop", 0x0019: "div0u",
         0x001B: "sleep"}

_STS0 = {0x02: "stc sr", 0x12: "stc gbr", 0x22: "stc vbr", 0x32: "stc ssr", 0x42: "stc spc",
         0x0A: "sts mach", 0x1A: "sts macl", 0x2A: "sts pr", 0x3A: "stc sgr",
         0x5A: "sts fpul", 0x6A: "sts fpscr", 0xFA: "stc dbr"}

_OP4 = {  # 0x4n?? single-register ops
    0x00: "shll {n}", 0x01: "shlr {n}", 0x04: "rotl {n}", 0x05: "rotr {n}",
    0x08: "shll2 {n}", 0x09: "shlr2 {n}", 0x10: "dt {n}", 0x11: "cmp/pz {n}",
    0x15: "cmp/pl {n}", 0x18: "shll8 {n}", 0x19: "shlr8 {n}", 0x1B: "tas.b @{n}",
    0x20: "shal {n}", 0x21: "shar {n}", 0x24: "rotcl {n}", 0x25: "rotcr {n}",
    0x28: "shll16 {n}", 0x29: "shlr16 {n}",
    0x02: "sts.l mach,@-{n}", 0x12: "sts.l macl,@-{n}", 0x22: "sts.l pr,@-{n}",
    0x52: "sts.l fpul,@-{n}", 0x62: "sts.l fpscr,@-{n}",
    0x03: "stc.l sr,@-{n}", 0x13: "stc.l gbr,@-{n}", 0x23: "stc.l vbr,@-{n}",
    0x32: "stc.l sgr,@-{n}", 0x33: "stc.l ssr,@-{n}", 0x43: "stc.l spc,@-{n}",
    0xF2: "stc.l dbr,@-{n}",
    0x06: "lds.l @{n}+,mach", 0x16: "lds.l @{n}+,macl", 0x26: "lds.l @{n}+,pr",
    0x56: "lds.l @{n}+,fpul", 0x66: "lds.l @{n}+,fpscr",
    0x07: "ldc.l @{n}+,sr", 0x17: "ldc.l @{n}+,gbr", 0x27: "ldc.l @{n}+,vbr",
    0x37: "ldc.l @{n}+,ssr", 0x47: "ldc.l @{n}+,spc", 0xF6: "ldc.l @{n}+,dbr",
    0x0A: "lds {n},mach", 0x1A: "lds {n},macl", 0x2A: "lds {n},pr",
    0x5A: "lds {n},fpul", 0x6A: "lds {n},fpscr",
    0x0E: "ldc {n},sr", 0x1E: "ldc {n},gbr", 0x2E: "ldc {n},vbr",
    0x3E: "ldc {n},ssr", 0x4E: "ldc {n},spc", 0xFA: "ldc {n},dbr",
}

_FPU2 = {0x0: "fadd", 0x1: "fsub", 0x2: "fmul", 0x3: "fdiv", 0x4: "fcmp/eq",
         0x5: "fcmp/gt", 0xC: "fmov"}

_FPU1 = {0x0: "fsts fpul,fr{n}", 0x1: "flds fr{n},fpul", 0x2: "float fpul,fr{n}",
         0x3: "ftrc fr{n},fpul", 0x4: "fneg fr{n}", 0x5: "fabs fr{n}",
         0x6: "fsqrt fr{n}", 0x7: "fsrra fr{n}", 0x8: "fldi0 fr{n}",
         0x9: "fldi1 fr{n}", 0xA: "fcnvsd fpul,dr{n}", 0xB: "fcnvds dr{n},fpul"}


def decode(op, pc):
    """Decode one 16-bit opcode located at pc."""
    hi, n, m, lo = op >> 12, (op >> 8) & 0xF, (op >> 4) & 0xF, op & 0xF
    rn, rm = f"r{n}", f"r{m}"
    i = Insn(pc, op, "")

    def done(text, **kw):
        i.text = text
        for k, v in kw.items():
            setattr(i, k, v)
        return i

    if (hi, lo) in _ALU2 and hi != 0x4 or (hi == 0x4 and lo in (0xC, 0xD)) or (hi == 0 and lo == 7):
        return done(f"{_ALU2[(hi, lo)]} {rm},{rn}")

    if hi == 0x0:
        if op in _SYS0:
            return done(_SYS0[op])
        if op == 0x000B:
            return done("rts", delay=True, ret=True, jump=True)
        if op == 0x002B:
            return done("rte", delay=True, ret=True, jump=True)
        if lo == 0x9 and m == 0x2:
            return done(f"movt {rn}")
        low8 = op & 0xFF
        if low8 in _STS0:
            return done(f"{_STS0[low8]},{rn}")
        if lo == 0x2 and m & 0x8:
            return done(f"stc r{m & 7}_bank,{rn}")
        if low8 == 0x03:
            return done(f"bsrf {rn}", delay=True, call=True, indirect=True)
        if low8 == 0x23:
            return done(f"braf {rn}", delay=True, jump=True, indirect=True)
        if low8 in (0x83, 0x93, 0xA3, 0xB3):
            return done(f"{ {0x83: 'pref', 0x93: 'ocbi', 0xA3: 'ocbp', 0xB3: 'ocbwb'}[low8]} @{rn}")
        if low8 == 0xC3:
            return done(f"movca.l r0,@{rn}")
        if lo in (4, 5, 6):
            return done(f"mov.{_SZ[lo - 4]} {rm},@(r0,{rn})")
        if lo in (0xC, 0xD, 0xE):
            return done(f"mov.{_SZ[lo - 0xC]} @(r0,{rm}),{rn}")
        if lo == 0xF:
            return done(f"mac.l @{rm}+,@{rn}+")
    elif hi == 0x1:
        return done(f"mov.l {rm},@({lo * 4},{rn})")
    elif hi == 0x2:
        if lo in (0, 1, 2):
            return done(f"mov.{_SZ[lo]} {rm},@{rn}")
        if lo in (4, 5, 6):
            return done(f"mov.{_SZ[lo - 4]} {rm},@-{rn}")
    elif hi == 0x4:
        low8 = op & 0xFF
        if low8 == 0x0B:
            return done(f"jsr @{rn}", delay=True, call=True, indirect=True)
        if low8 == 0x2B:
            return done(f"jmp @{rn}", delay=True, jump=True, indirect=True)
        if low8 in _OP4:
            return done(_OP4[low8].format(n=rn))
        if lo == 0x3 and m & 0x8:
            return done(f"stc.l r{m & 7}_bank,@-{rn}")
        if lo == 0x7 and m & 0x8:
            return done(f"ldc.l @{rn}+,r{m & 7}_bank")
        if lo == 0xE and m & 0x8:
            return done(f"ldc {rn},r{m & 7}_bank")
        if lo == 0xF:
            return done(f"mac.w @{rm}+,@{rn}+")
    elif hi == 0x5:
        return done(f"mov.l @({lo * 4},{rm}),{rn}")
    elif hi == 0x6:
        if lo in (0, 1, 2):
            return done(f"mov.{_SZ[lo]} @{rm},{rn}")
        if lo in (4, 5, 6):
            return done(f"mov.{_SZ[lo - 4]} @{rm}+,{rn}")
    elif hi == 0x7:
        return done(f"add #{s8(op & 0xFF)},{rn}")
    elif hi == 0x8:
        d = op & 0xF
        if n == 0x0:
            return done(f"mov.b r0,@({d},{rm})")
        if n == 0x1:
            return done(f"mov.w r0,@({d * 2},{rm})")
        if n == 0x4:
            return done(f"mov.b @({d},{rm}),r0")
        if n == 0x5:
            return done(f"mov.w @({d * 2},{rm}),r0")
        if n == 0x8:
            return done(f"cmp/eq #{s8(op & 0xFF)},r0")
        if n in (0x9, 0xB, 0xD, 0xF):
            t = pc + 4 + s8(op & 0xFF) * 2
            name = {0x9: "bt", 0xB: "bf", 0xD: "bt/s", 0xF: "bf/s"}[n]
            return done(f"{name} 0x{t:08x}", target=t, cond=True, delay=n in (0xD, 0xF))
    elif hi == 0x9:
        a = pc + 4 + (op & 0xFF) * 2
        return done(f"mov.w @(0x{a:08x}),{rn}", lit=a, lit_size=2)
    elif hi in (0xA, 0xB):
        t = pc + 4 + s12(op & 0xFFF) * 2
        if hi == 0xA:
            return done(f"bra 0x{t:08x}", target=t, delay=True, jump=True)
        return done(f"bsr 0x{t:08x}", target=t, delay=True, call=True)
    elif hi == 0xC:
        d = op & 0xFF
        sub = n
        if sub in (0, 1, 2):
            return done(f"mov.{_SZ[sub]} r0,@({d << sub},gbr)")
        if sub == 3:
            return done(f"trapa #{d}")
        if sub in (4, 5, 6):
            return done(f"mov.{_SZ[sub - 4]} @({d << (sub - 4)},gbr),r0")
        if sub == 7:
            a = (pc & ~3) + 4 + d * 4
            return done(f"mova @(0x{a:08x}),r0", lit=a, lit_size=0)
        names = {8: "tst", 9: "and", 0xA: "xor", 0xB: "or"}
        if sub in names:
            return done(f"{names[sub]} #{d},r0")
        names = {0xC: "tst.b", 0xD: "and.b", 0xE: "xor.b", 0xF: "or.b"}
        return done(f"{names[sub]} #{d},@(r0,gbr)")
    elif hi == 0xD:
        a = (pc & ~3) + 4 + (op & 0xFF) * 4
        return done(f"mov.l @(0x{a:08x}),{rn}", lit=a, lit_size=4)
    elif hi == 0xE:
        return done(f"mov #{s8(op & 0xFF)},{rn}")
    elif hi == 0xF:
        if lo in _FPU2:
            return done(f"{_FPU2[lo]} fr{m},fr{n}")
        if lo == 0x6:
            return done(f"fmov.s @(r0,{rm}),fr{n}")
        if lo == 0x7:
            return done(f"fmov.s fr{m},@(r0,{rn})")
        if lo == 0x8:
            return done(f"fmov.s @{rm},fr{n}")
        if lo == 0x9:
            return done(f"fmov.s @{rm}+,fr{n}")
        if lo == 0xA:
            return done(f"fmov.s fr{m},@{rn}")
        if lo == 0xB:
            return done(f"fmov.s fr{m},@-{rn}")
        if lo == 0xE:
            return done(f"fmac fr0,fr{m},fr{n}")
        if lo == 0xD:
            if m in _FPU1:
                return done(_FPU1[m].format(n=n))
            if m == 0xE:
                return done(f"fipr fv{(n & 3) * 4},fv{(n >> 2) * 4}")
            if m == 0xF:
                if op == 0xF3FD:
                    return done("fschg")
                if op == 0xFBFD:
                    return done("frchg")
                if n & 1 == 0:
                    return done(f"fsca fpul,dr{n}")
                if n & 3 == 1:
                    return done(f"ftrv xmtrx,fv{(n >> 2) * 4}")
    return done(f".word 0x{op:04x}", valid=False)
