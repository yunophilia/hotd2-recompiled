"""SH-4 -> C static recompiler for the HOTD2 program image.

Each function found by funcs.discover() becomes `void f_XXXXXXXX(Sh4 *c)`.
Branches inside a function become gotos, known calls become direct C calls,
and anything only known at run time goes through sh4_dispatch().

Output goes to out/gen/ and is derived from the game's code: never commit it.

    python3 -I tools/recomp.py <hotd2.zip> out/gen [--per-file 200]
"""
import argparse
import os
import zipfile

import funcs
from sh4 import decode, s8, s12


def hx(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


class Gen:
    def __init__(self, img, fns):
        self.img = img
        self.fns = fns
        self.unhandled = {}

    # ---------- single (non-branch) instruction semantics ----------
    def sem(self, pc):
        img = self.img
        op = img.u16(pc)
        hi, n, m, lo = op >> 12, (op >> 8) & 0xF, (op >> 4) & 0xF, op & 0xF
        R = lambda x: f"c->r[{x}]"
        Rn, Rm = R(n), R(m)
        FR = lambda x: f"c->fr.f[{x}]"
        FU = lambda x: f"c->fr.u[{x}]"
        imm8 = op & 0xFF
        d4 = op & 0xF
        low8 = op & 0xFF
        PR = "(c->fpscr & FPSCR_PR)"

        if hi == 0x0:
            simple = {0x0008: "c->t = 0;", 0x0018: "c->t = 1;", 0x0028: "c->mach = c->macl = 0;",
                      0x0048: "c->s = 0;", 0x0058: "c->s = 1;", 0x0009: ";",
                      0x0019: "c->m = c->q = c->t = 0;", 0x0038: ";"}
            if op in simple:
                return simple[op]
            if lo == 0x9 and m == 0x2:
                return f"{Rn} = c->t;"
            sts = {0x02: "sr_get(c)", 0x12: "c->gbr", 0x22: "c->vbr", 0x32: "c->ssr",
                   0x42: "c->spc", 0x0A: "c->mach", 0x1A: "c->macl", 0x2A: "c->pr",
                   0x3A: "c->sgr", 0x5A: "c->fpul", 0x6A: "c->fpscr", 0xFA: "c->dbr"}
            if low8 in sts:
                return f"{Rn} = {sts[low8]};"
            if lo == 0x2 and m & 0x8:
                return f"{Rn} = c->r_bank[{m & 7}];"
            if low8 == 0x83:
                return f"op_pref(c, {Rn});"
            if low8 in (0x93, 0xA3, 0xB3):
                return ";  /* cache op */"
            if low8 == 0xC3:
                return f"wr32({Rn}, c->r[0]);"
            if lo == 0x4:
                return f"wr8({Rn} + c->r[0], (u8){Rm});"
            if lo == 0x5:
                return f"wr16({Rn} + c->r[0], (u16){Rm});"
            if lo == 0x6:
                return f"wr32({Rn} + c->r[0], {Rm});"
            if lo == 0x7:
                return f"c->macl = {Rn} * {Rm};"
            if lo == 0xC:
                return f"{Rn} = (u32)(s32)(s8)rd8({Rm} + c->r[0]);"
            if lo == 0xD:
                return f"{Rn} = (u32)(s32)(s16)rd16({Rm} + c->r[0]);"
            if lo == 0xE:
                return f"{Rn} = rd32({Rm} + c->r[0]);"
            if lo == 0xF:
                return f"op_mac_l(c, {n}, {m});"
        elif hi == 0x1:
            return f"wr32({Rn} + {d4 * 4}, {Rm});"
        elif hi == 0x2:
            st = {0: ("wr8", "(u8)", 1), 1: ("wr16", "(u16)", 2), 2: ("wr32", "", 4)}
            if lo in st:
                f, cast, _ = st[lo]
                return f"{f}({Rn}, {cast}{Rm});"
            if lo in (4, 5, 6):
                f, cast, sz = st[lo - 4]
                return f"{{ u32 v = {Rm}; {Rn} -= {sz}; {f}({Rn}, {cast}v); }}"
            alu = {
                0x7: f"c->q = {Rn} >> 31; c->m = {Rm} >> 31; c->t = c->q ^ c->m;",
                0x8: f"c->t = ({Rn} & {Rm}) == 0;",
                0x9: f"{Rn} &= {Rm};", 0xA: f"{Rn} ^= {Rm};", 0xB: f"{Rn} |= {Rm};",
                0xC: f"{{ u32 x = {Rn} ^ {Rm}; c->t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u); }}",
                0xD: f"{Rn} = ({Rn} >> 16) | ({Rm} << 16);",
                0xE: f"c->macl = (u32)(u16){Rn} * (u32)(u16){Rm};",
                0xF: f"c->macl = (u32)((s32)(s16){Rn} * (s32)(s16){Rm});",
            }
            if lo in alu:
                return alu[lo]
        elif hi == 0x3:
            alu = {
                0x0: f"c->t = {Rn} == {Rm};",
                0x2: f"c->t = {Rn} >= {Rm};",
                0x3: f"c->t = (s32){Rn} >= (s32){Rm};",
                0x4: f"op_div1(c, {n}, {m});",
                0x5: f"{{ u64 p = (u64){Rn} * {Rm}; c->mach = (u32)(p >> 32); c->macl = (u32)p; }}",
                0x6: f"c->t = {Rn} > {Rm};",
                0x7: f"c->t = (s32){Rn} > (s32){Rm};",
                0x8: f"{Rn} -= {Rm};",
                0xA: f"{{ u32 a = {Rn}, b = {Rm}, d = a - b, r = d - c->t; c->t = (a < d) | (d < r); {Rn} = r; }}",
                0xB: f"{{ s32 a = (s32){Rn}, b = (s32){Rm}; s32 r = (s32)((u32)a - (u32)b); c->t = ((a ^ b) & (a ^ r)) < 0; {Rn} = (u32)r; }}",
                0xC: f"{Rn} += {Rm};",
                0xD: f"{{ s64 p = (s64)(s32){Rn} * (s32){Rm}; c->mach = (u32)((u64)p >> 32); c->macl = (u32)p; }}",
                0xE: f"{{ u32 a = {Rn}, s = a + {Rm}, r = s + c->t; c->t = (a > s) | (s > r); {Rn} = r; }}",
                0xF: f"{{ s32 a = (s32){Rn}, b = (s32){Rm}; s32 r = (s32)((u32)a + (u32)b); c->t = (~(a ^ b) & (a ^ r)) < 0; {Rn} = (u32)r; }}",
            }
            if lo in alu:
                return alu[lo]
        elif hi == 0x4:
            one = {
                0x00: f"c->t = {Rn} >> 31; {Rn} <<= 1;",
                0x01: f"c->t = {Rn} & 1; {Rn} >>= 1;",
                0x04: f"c->t = {Rn} >> 31; {Rn} = ({Rn} << 1) | c->t;",
                0x05: f"c->t = {Rn} & 1; {Rn} = ({Rn} >> 1) | (c->t << 31);",
                0x08: f"{Rn} <<= 2;", 0x09: f"{Rn} >>= 2;",
                0x10: f"{Rn} -= 1; c->t = {Rn} == 0;",
                0x11: f"c->t = (s32){Rn} >= 0;",
                0x15: f"c->t = (s32){Rn} > 0;",
                0x18: f"{Rn} <<= 8;", 0x19: f"{Rn} >>= 8;",
                0x1B: f"{{ u8 v = rd8({Rn}); c->t = v == 0; wr8({Rn}, v | 0x80); }}",
                0x20: f"c->t = {Rn} >> 31; {Rn} <<= 1;",
                0x21: f"c->t = {Rn} & 1; {Rn} = (u32)((s32){Rn} >> 1);",
                0x24: f"{{ u32 t = {Rn} >> 31; {Rn} = ({Rn} << 1) | c->t; c->t = t; }}",
                0x25: f"{{ u32 t = {Rn} & 1; {Rn} = ({Rn} >> 1) | (c->t << 31); c->t = t; }}",
                0x28: f"{Rn} <<= 16;", 0x29: f"{Rn} >>= 16;",
                # sts.l / stc.l (pre-decrement store)
                0x02: f"{Rn} -= 4; wr32({Rn}, c->mach);",
                0x12: f"{Rn} -= 4; wr32({Rn}, c->macl);",
                0x22: f"{Rn} -= 4; wr32({Rn}, c->pr);",
                0x52: f"{Rn} -= 4; wr32({Rn}, c->fpul);",
                0x62: f"{Rn} -= 4; wr32({Rn}, c->fpscr);",
                0x03: f"{Rn} -= 4; wr32({Rn}, sr_get(c));",
                0x13: f"{Rn} -= 4; wr32({Rn}, c->gbr);",
                0x23: f"{Rn} -= 4; wr32({Rn}, c->vbr);",
                0x32: f"{Rn} -= 4; wr32({Rn}, c->sgr);",
                0x33: f"{Rn} -= 4; wr32({Rn}, c->ssr);",
                0x43: f"{Rn} -= 4; wr32({Rn}, c->spc);",
                0xF2: f"{Rn} -= 4; wr32({Rn}, c->dbr);",
                # lds.l / ldc.l (post-increment load)
                0x06: f"c->mach = rd32({Rn}); {Rn} += 4;",
                0x16: f"c->macl = rd32({Rn}); {Rn} += 4;",
                0x26: f"c->pr = rd32({Rn}); {Rn} += 4;",
                0x56: f"c->fpul = rd32({Rn}); {Rn} += 4;",
                0x66: f"fpscr_set(c, rd32({Rn})); {Rn} += 4;",
                0x07: f"sr_set(c, rd32({Rn})); {Rn} += 4;",
                0x17: f"c->gbr = rd32({Rn}); {Rn} += 4;",
                0x27: f"c->vbr = rd32({Rn}); {Rn} += 4;",
                0x37: f"c->ssr = rd32({Rn}); {Rn} += 4;",
                0x47: f"c->spc = rd32({Rn}); {Rn} += 4;",
                0xF6: f"c->dbr = rd32({Rn}); {Rn} += 4;",
                0x0A: f"c->mach = {Rn};", 0x1A: f"c->macl = {Rn};", 0x2A: f"c->pr = {Rn};",
                0x5A: f"c->fpul = {Rn};", 0x6A: f"fpscr_set(c, {Rn});",
                0x0E: f"sr_set(c, {Rn});", 0x1E: f"c->gbr = {Rn};", 0x2E: f"c->vbr = {Rn};",
                0x3E: f"c->ssr = {Rn};", 0x4E: f"c->spc = {Rn};", 0xFA: f"c->dbr = {Rn};",
            }
            if low8 in one:
                return one[low8]
            if lo == 0x3 and m & 0x8:
                return f"{Rn} -= 4; wr32({Rn}, c->r_bank[{m & 7}]);"
            if lo == 0x7 and m & 0x8:
                return f"c->r_bank[{m & 7}] = rd32({Rn}); {Rn} += 4;"
            if lo == 0xE and m & 0x8:
                return f"c->r_bank[{m & 7}] = {Rn};"
            if lo == 0xC:
                return f"{Rn} = op_shad({Rn}, {Rm});"
            if lo == 0xD:
                return f"{Rn} = op_shld({Rn}, {Rm});"
            if lo == 0xF:
                return f"op_mac_w(c, {n}, {m});"
        elif hi == 0x5:
            return f"{Rn} = rd32({Rm} + {d4 * 4});"
        elif hi == 0x6:
            ld = {0: "(u32)(s32)(s8)rd8", 1: "(u32)(s32)(s16)rd16", 2: "rd32"}
            if lo in ld:
                return f"{Rn} = {ld[lo]}({Rm});"
            if lo in (4, 5, 6):
                sz = 1 << (lo - 4)
                inc = "" if n == m else f" {Rm} += {sz};"
                return f"{Rn} = {ld[lo - 4]}({Rm});{inc}"
            una = {
                0x3: f"{Rn} = {Rm};", 0x7: f"{Rn} = ~{Rm};",
                0x8: f"{Rn} = ({Rm} & 0xFFFF0000u) | (({Rm} & 0xFF) << 8) | (({Rm} >> 8) & 0xFF);",
                0x9: f"{Rn} = ({Rm} << 16) | ({Rm} >> 16);",
                0xA: f"{{ u32 t0 = 0u - {Rm}; u32 r = t0 - c->t; c->t = (0u < t0) | (t0 < r); {Rn} = r; }}",
                0xB: f"{Rn} = 0u - {Rm};",
                0xC: f"{Rn} = (u8){Rm};", 0xD: f"{Rn} = (u16){Rm};",
                0xE: f"{Rn} = (u32)(s32)(s8){Rm};", 0xF: f"{Rn} = (u32)(s32)(s16){Rm};",
            }
            if lo in una:
                return una[lo]
        elif hi == 0x7:
            return f"{Rn} += (u32){s8(imm8)};"
        elif hi == 0x8:
            if n == 0x0:
                return f"wr8({Rm} + {d4}, (u8)c->r[0]);"
            if n == 0x1:
                return f"wr16({Rm} + {d4 * 2}, (u16)c->r[0]);"
            if n == 0x4:
                return f"c->r[0] = (u32)(s32)(s8)rd8({Rm} + {d4});"
            if n == 0x5:
                return f"c->r[0] = (u32)(s32)(s16)rd16({Rm} + {d4 * 2});"
            if n == 0x8:
                return f"c->t = c->r[0] == (u32){s8(imm8)};"
        elif hi == 0x9:
            a = pc + 4 + imm8 * 2
            return f"{Rn} = {hx(self.lit16s(a))};  /* @{a:08x} */"
        elif hi == 0xC:
            sub = n
            if sub in (0, 1, 2):
                f = ("wr8", "wr16", "wr32")[sub]
                cast = ("(u8)", "(u16)", "")[sub]
                return f"{f}(c->gbr + {imm8 << sub}, {cast}c->r[0]);"
            if sub in (4, 5, 6):
                f = ("(u32)(s32)(s8)rd8", "(u32)(s32)(s16)rd16", "rd32")[sub - 4]
                return f"c->r[0] = {f}(c->gbr + {imm8 << (sub - 4)});"
            if sub == 7:
                return f"c->r[0] = {hx((pc & ~3) + 4 + imm8 * 4)};"
            if sub == 8:
                return f"c->t = (c->r[0] & {imm8}u) == 0;"
            if sub in (9, 0xA, 0xB):
                o = {9: "&", 0xA: "^", 0xB: "|"}[sub]
                return f"c->r[0] {o}= {imm8}u;"
            if sub == 0xC:
                return f"c->t = (rd8(c->gbr + c->r[0]) & {imm8}u) == 0;"
            if sub in (0xD, 0xE, 0xF):
                o = {0xD: "&", 0xE: "^", 0xF: "|"}[sub]
                return f"{{ u32 a = c->gbr + c->r[0]; wr8(a, (u8)(rd8(a) {o} {imm8}u)); }}"
        elif hi == 0xD:
            a = (pc & ~3) + 4 + imm8 * 4
            return f"{Rn} = {hx(self.img.u32(a) if self.img.has(a, 4) else 0)};  /* @{a:08x} */"
        elif hi == 0xE:
            return f"{Rn} = (u32){s8(imm8)};"
        elif hi == 0xF:
            dbl = lambda expr_d, expr_s: f"if {PR} {{ {expr_d} }} else {{ {expr_s} }}"
            dn, dm = n & 14, m & 14
            ar = {0x0: "+", 0x1: "-", 0x2: "*", 0x3: "/"}
            if lo in ar:
                o = ar[lo]
                return dbl(f"dr_set(c, {dn}, dr_get(c, {dn}) {o} dr_get(c, {dm}));",
                           f"{FR(n)} = {FR(n)} {o} {FR(m)};")
            if lo == 0x4:
                return dbl(f"c->t = dr_get(c, {dn}) == dr_get(c, {dm});", f"c->t = {FR(n)} == {FR(m)};")
            if lo == 0x5:
                return dbl(f"c->t = dr_get(c, {dn}) > dr_get(c, {dm});", f"c->t = {FR(n)} > {FR(m)};")
            if lo == 0x6:
                return f"fmov_load(c, {n}, {Rm} + c->r[0]);"
            if lo == 0x7:
                return f"fmov_store(c, {m}, {Rn} + c->r[0]);"
            if lo == 0x8:
                return f"fmov_load(c, {n}, {Rm});"
            if lo == 0x9:
                return f"fmov_load(c, {n}, {Rm}); {Rm} += fmov_size(c);"
            if lo == 0xA:
                return f"fmov_store(c, {m}, {Rn});"
            if lo == 0xB:
                return f"{Rn} -= fmov_size(c); fmov_store(c, {m}, {Rn});"
            if lo == 0xC:
                return f"fmov_rr(c, {n}, {m});"
            if lo == 0xE:
                return f"{FR(n)} = {FR(0)} * {FR(m)} + {FR(n)};"
            if lo == 0xD:
                u = {
                    0x0: f"{FU(n)} = c->fpul;",
                    0x1: f"c->fpul = {FU(n)};",
                    0x2: dbl(f"dr_set(c, {dn}, (double)(s32)c->fpul);", f"{FR(n)} = (float)(s32)c->fpul;"),
                    0x3: dbl(f"c->fpul = ftrc_sat(dr_get(c, {dn}));", f"c->fpul = ftrc_sat({FR(n)});"),
                    0x4: f"{FU(n)} ^= 0x80000000u;",  # also correct for DRn (sign in high word)
                    0x5: f"{FU(n)} &= 0x7FFFFFFFu;",
                    0x6: dbl(f"dr_set(c, {dn}, sqrt(dr_get(c, {dn})));", f"{FR(n)} = sqrtf({FR(n)});"),
                    0x7: f"{FR(n)} = 1.0f / sqrtf({FR(n)});",
                    0x8: f"{FU(n)} = 0;",
                    0x9: f"{FU(n)} = 0x3F800000u;",
                    0xA: f"{{ float s; memcpy(&s, &c->fpul, 4); dr_set(c, {dn}, s); }}",
                    0xB: f"{{ float s = (float)dr_get(c, {dn}); memcpy(&c->fpul, &s, 4); }}",
                }
                if m in u:
                    return u[m]
                if m == 0xE:
                    return f"op_fipr(c, {(n & 3) * 4}, {(n >> 2) * 4});"
                if m == 0xF:
                    if op == 0xF3FD:
                        return "c->fpscr ^= FPSCR_SZ;"
                    if op == 0xFBFD:
                        return "fpscr_set(c, c->fpscr ^ FPSCR_FR);"
                    if n & 1 == 0:
                        return f"op_fsca(c, {n});"
                    if n & 3 == 1:
                        return f"op_ftrv(c, {(n >> 2) * 4});"
        self.unhandled[op] = pc
        return f"sh4_unimplemented(c, {hx(pc)}, 0x{op:04X});"

    def lit16s(self, a):
        v = self.img.u16(a) if self.img.has(a) else 0
        return v - 0x10000 if v & 0x8000 else v

    # ---------- function body ----------
    def call_expr(self, target):
        if target in self.fns:
            return f"f_{target:08x}(c);"
        return f"sh4_dispatch(c, {hx(target)});"

    def function(self, f):
        img = self.img
        body, _ = self.fns[f]
        pcs = sorted(body)
        bodyset = set(pcs)
        labels, out = {f}, []
        # first pass: collect branch targets that need labels
        for pc in pcs:
            i = decode(img.u16(pc), pc)
            if i.target is not None and not i.call and i.target in bodyset:
                labels.add(i.target)
            if i.cond and i.delay and pc + 4 in bodyset:
                labels.add(pc + 4)
            if i.text.startswith("braf"):
                labels.update(t for t in funcs.switch_targets(img, pc) if t in bodyset)
        delay_of = set()
        prev_end = None
        run = 0  # instructions since the last TICK
        for pc in pcs:
            if pc in delay_of:  # emitted with its branch (inline, labelled if needed)
                continue
            i = decode(img.u16(pc), pc)
            if prev_end is not None and pc != prev_end:
                out.append(f"\t/* gap: fallthrough from {prev_end:08x} */ sh4_dispatch(c, {hx(prev_end)}); return;")
            if pc in labels:
                out.append(f"L_{pc:08x}:;")
            prev_end = pc + 2
            run += 1
            if not i.delay and not i.cond:
                if i.text.startswith("trapa") or i.text.startswith("sleep") or not i.valid:
                    out.append(f"\tsh4_unimplemented(c, {hx(pc)}, 0x{i.op:04X});")
                else:
                    out.append(f"\t{self.sem(pc)}")
                continue
            if i.cond and not i.delay:  # bt / bf
                cond = "c->t" if i.text.startswith("bt") else "!c->t"
                out.append(f"\tTICK(c, {run}); if ({cond}) {self.goto(i.target, bodyset)}")
                run = 0
                continue
            # delayed branches: capture target/condition, run the slot, then transfer
            slot_pc = pc + 2
            delay_of.add(slot_pc)
            slot = f"TICK(c, {run + 1}); /* slot */ {self.sem(slot_pc)}"
            run = 0
            prev_end = pc + 4
            name = i.text.split()[0]
            rn = (i.op >> 8) & 0xF
            if name in ("bt/s", "bf/s"):
                cond = "c->t" if name == "bt/s" else "!c->t"
                nxt = (f"goto L_{pc + 4:08x};" if pc + 4 in bodyset
                       else f"{{ sh4_dispatch(c, {hx(pc + 4)}); return; }}")
                out.append(f"\t{{ u32 cond = {cond}; {slot} if (cond) {self.goto(i.target, bodyset)} {nxt} }}")
                if slot_pc in labels:  # someone jumps straight into the slot
                    out.append(f"L_{slot_pc:08x}:; \t{self.sem(slot_pc)} {nxt}")
                prev_end = None
                continue
            if name == "bra":
                out.append(f"\t{{ {slot} {self.goto(i.target, bodyset)} }}")
                prev_end = None
            elif name == "bsr":
                out.append(f"\t{{ c->pr = {hx(pc + 4)}; {slot} {self.call_expr(i.target)} }}")
            elif name == "jsr":
                t = funcs.reg_literal(img, pc, f"r{rn}")
                if t == CORO_SAVE:
                    # host setjmp in this frame, so a later restore-context resumes right here
                    direct = f"if (tgt == {hx(t)}) {{ CORO_SAVE(c); }} else "
                elif t in self.fns:
                    direct = f"if (tgt == {hx(t)}) f_{t:08x}(c); else "
                else:
                    direct = ""
                out.append(f"\t{{ u32 tgt = c->r[{rn}]; c->pr = {hx(pc + 4)}; {slot} {direct}sh4_dispatch(c, tgt); }}")
            elif name == "bsrf":
                out.append(f"\t{{ u32 tgt = {hx(pc + 4)} + c->r[{rn}]; c->pr = {hx(pc + 4)}; {slot} sh4_dispatch(c, tgt); }}")
            elif name == "rts":
                out.append(f"\t{{ {slot} return; }}")
                prev_end = None
            elif name == "rte":
                out.append(f"\t{{ {slot} sr_set(c, c->ssr); return; }}")
                prev_end = None
            elif name == "jmp":
                t = funcs.reg_literal(img, pc, f"r{rn}")
                direct = f"if (tgt == {hx(t)}) {{ f_{t:08x}(c); return; }} " if t in self.fns else ""
                out.append(f"\t{{ u32 tgt = c->r[{rn}]; {slot} {direct}sh4_dispatch(c, tgt); return; }}")
                prev_end = None
            elif name == "braf":
                cases = " ".join(f"case {hx(t)}: goto L_{t:08x};"
                                 for t in sorted(set(funcs.switch_targets(img, pc))) if t in bodyset)
                out.append(f"\t{{ u32 tgt = {hx(pc + 4)} + c->r[{rn}]; {slot} switch (tgt) {{ {cases} default: sh4_dispatch(c, tgt); return; }} }}")
                prev_end = None
            if slot_pc in labels and name not in ("bt/s", "bf/s"):
                out.append(f"L_{slot_pc:08x}:; \t{self.sem(slot_pc)}")
                prev_end = pc + 4
        if prev_end is not None:
            out.append(f"\tsh4_dispatch(c, {hx(prev_end)}); /* ran off the end */")
        if pcs and pcs[0] != f:
            # the body reaches code below the entry point: start at the entry, not the lowest address
            out.insert(0, f"\tgoto L_{f:08x};")
        if f not in (CORO_SAVE, CORO_RESTORE):
            out.insert(0, f"	DIFF_ENTER(c, {hx(f)}, f_{f:08x});")
        if f == CORO_RESTORE:
            # keep the original register reload, then switch fibers natively (never returns)
            return (f"static void f_{f:08x}_load(Sh4 *c)\n{{\n" + "\n".join(out) + "\n}\n"
                    f"void f_{f:08x}(Sh4 *c)\n{{\n\tu32 buf = c->r[4];\n"
                    f"\tf_{f:08x}_load(c);\n\tcoro_restore(c, buf);\n}}\n")
        return f"void f_{f:08x}(Sh4 *c)\n{{\n" + "\n".join(out) + "\n}\n"

    def goto(self, target, bodyset):
        if target in bodyset:
            return f"goto L_{target:08x};"
        return f"{{ {self.call_expr(target)} return; }}"


HEADER = '#include "sh4ctx.h"\n#include "sh4ops.h"\n#include "coro.h"\n#include "funcs.h"\n\n'

# HOTD2's task switch primitives (see runtime/coro.h)
CORO_SAVE = 0x0C0BA0C0
CORO_RESTORE = 0x0C0BA126


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("outdir")
    ap.add_argument("--per-file", type=int, default=200)
    a = ap.parse_args()
    data = zipfile.ZipFile(a.zip).read("epr-21585.ic22")[:funcs.PROGRAM_SIZE]
    img = funcs.Image(data)
    fns = funcs.discover(img)
    g = Gen(img, fns)
    os.makedirs(a.outdir, exist_ok=True)
    order = sorted(fns)
    with open(os.path.join(a.outdir, "funcs.h"), "w") as h:
        h.write("#pragma once\n#include \"sh4ctx.h\"\n")
        for f in order:
            h.write(f"void f_{f:08x}(Sh4 *c);\n")
        h.write("typedef struct { u32 addr; void (*fn)(Sh4 *); } FuncEntry;\n")
        h.write("extern const FuncEntry func_table[];\nextern const u32 func_count;\n")
    for k in range(0, len(order), a.per_file):
        with open(os.path.join(a.outdir, f"gen_{k // a.per_file:03d}.c"), "w") as out:
            out.write(HEADER)
            for f in order[k:k + a.per_file]:
                out.write(g.function(f) + "\n")
    with open(os.path.join(a.outdir, "table.c"), "w") as t:
        t.write('#include "funcs.h"\nconst FuncEntry func_table[] = {\n')
        for f in order:
            t.write(f"\t{{ 0x{f:08X}u, f_{f:08x} }},\n")
        t.write(f"}};\nconst u32 func_count = {len(order)};\n")
    print(f"{len(order)} functions -> {a.outdir}; unhandled opcodes: {len(g.unhandled)}")
    for op, pc in sorted(g.unhandled.items())[:20]:
        print(f"  {op:04x} first at {pc:08x}: {decode(op, pc).text}")


if __name__ == "__main__":
    main()
