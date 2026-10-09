/* SH-4 interpreter, used as a fallback for code the static recompiler never
 * saw (sh4_dispatch with an unknown target) and as a reference to check the
 * recompiled code against.
 *
 * sh4_interp(c, entry) runs from entry until the function returns (rts/rte at
 * call depth 0). Calls to addresses that have recompiled code go to that code;
 * other calls are interpreted recursively.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh4ctx.h"
#include "sh4ops.h"
#include "coro.h"

/* provided by the host (harness.c / web.c): compiled function for addr, or NULL */
void (*sh4_lookup(u32 addr))(Sh4 *);
u64 interp_instructions;

static void call(Sh4 *c, u32 target);
void sh4_interp(Sh4 *c, u32 pc);

static inline s32 s8v(u32 x) { return (s32)(s8)(x & 0xFF); }
static inline s32 s12v(u32 x) { return (x & 0x800) ? (s32)(x | 0xFFFFF000u) : (s32)x; }

/* Execute one non-branch instruction. Returns 0 if it was a branch (not handled). */
static int exec(Sh4 *c, u32 pc, u16 op)
{
	u32 n = (op >> 8) & 15, m = (op >> 4) & 15, lo = op & 15, imm = op & 0xFF;
	u32 *R = c->r;
	switch (op >> 12) {
	case 0x0:
		switch (op) {
		case 0x0008: c->t = 0; return 1;
		case 0x0018: c->t = 1; return 1;
		case 0x0028: c->mach = c->macl = 0; return 1;
		case 0x0048: c->s = 0; return 1;
		case 0x0058: c->s = 1; return 1;
		case 0x0009: case 0x0038: return 1;
		case 0x0019: c->m = c->q = c->t = 0; return 1;
		}
		switch (op & 0xFF) {
		case 0x29: R[n] = c->t; return 1;
		case 0x02: R[n] = sr_get(c); return 1;
		case 0x12: R[n] = c->gbr; return 1;
		case 0x22: R[n] = c->vbr; return 1;
		case 0x32: R[n] = c->ssr; return 1;
		case 0x42: R[n] = c->spc; return 1;
		case 0x0A: R[n] = c->mach; return 1;
		case 0x1A: R[n] = c->macl; return 1;
		case 0x2A: R[n] = c->pr; return 1;
		case 0x3A: R[n] = c->sgr; return 1;
		case 0x5A: R[n] = c->fpul; return 1;
		case 0x6A: R[n] = c->fpscr; return 1;
		case 0xFA: R[n] = c->dbr; return 1;
		case 0x83: op_pref(c, R[n]); return 1;
		case 0x93: case 0xA3: case 0xB3: return 1;
		case 0xC3: wr32(R[n], R[0]); return 1;
		}
		if (lo == 2 && (m & 8)) { R[n] = c->r_bank[m & 7]; return 1; }
		switch (lo) {
		case 4: wr8(R[n] + R[0], (u8)R[m]); return 1;
		case 5: wr16(R[n] + R[0], (u16)R[m]); return 1;
		case 6: wr32(R[n] + R[0], R[m]); return 1;
		case 7: c->macl = R[n] * R[m]; return 1;
		case 0xC: R[n] = (u32)(s32)(s8)rd8(R[m] + R[0]); return 1;
		case 0xD: R[n] = (u32)(s32)(s16)rd16(R[m] + R[0]); return 1;
		case 0xE: R[n] = rd32(R[m] + R[0]); return 1;
		case 0xF: op_mac_l(c, (int)n, (int)m); return 1;
		}
		break;
	case 0x1: wr32(R[n] + lo * 4, R[m]); return 1;
	case 0x2:
		switch (lo) {
		case 0: wr8(R[n], (u8)R[m]); return 1;
		case 1: wr16(R[n], (u16)R[m]); return 1;
		case 2: wr32(R[n], R[m]); return 1;
		case 4: { u32 v = R[m]; R[n] -= 1; wr8(R[n], (u8)v); return 1; }
		case 5: { u32 v = R[m]; R[n] -= 2; wr16(R[n], (u16)v); return 1; }
		case 6: { u32 v = R[m]; R[n] -= 4; wr32(R[n], v); return 1; }
		case 7: c->q = R[n] >> 31; c->m = R[m] >> 31; c->t = c->q ^ c->m; return 1;
		case 8: c->t = (R[n] & R[m]) == 0; return 1;
		case 9: R[n] &= R[m]; return 1;
		case 0xA: R[n] ^= R[m]; return 1;
		case 0xB: R[n] |= R[m]; return 1;
		case 0xC: { u32 x = R[n] ^ R[m]; c->t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u); return 1; }
		case 0xD: R[n] = (R[n] >> 16) | (R[m] << 16); return 1;
		case 0xE: c->macl = (u32)(u16)R[n] * (u32)(u16)R[m]; return 1;
		case 0xF: c->macl = (u32)((s32)(s16)R[n] * (s32)(s16)R[m]); return 1;
		}
		break;
	case 0x3:
		switch (lo) {
		case 0: c->t = R[n] == R[m]; return 1;
		case 2: c->t = R[n] >= R[m]; return 1;
		case 3: c->t = (s32)R[n] >= (s32)R[m]; return 1;
		case 4: op_div1(c, (int)n, (int)m); return 1;
		case 5: { u64 p = (u64)R[n] * R[m]; c->mach = (u32)(p >> 32); c->macl = (u32)p; return 1; }
		case 6: c->t = R[n] > R[m]; return 1;
		case 7: c->t = (s32)R[n] > (s32)R[m]; return 1;
		case 8: R[n] -= R[m]; return 1;
		case 0xA: { u32 a = R[n], b = R[m], d = a - b, r = d - c->t; c->t = (a < d) | (d < r); R[n] = r; return 1; }
		case 0xB: { s32 a = (s32)R[n], b = (s32)R[m]; s32 r = (s32)((u32)a - (u32)b); c->t = ((a ^ b) & (a ^ r)) < 0; R[n] = (u32)r; return 1; }
		case 0xC: R[n] += R[m]; return 1;
		case 0xD: { s64 p = (s64)(s32)R[n] * (s32)R[m]; c->mach = (u32)((u64)p >> 32); c->macl = (u32)p; return 1; }
		case 0xE: { u32 a = R[n], s = a + R[m], r = s + c->t; c->t = (a > s) | (s > r); R[n] = r; return 1; }
		case 0xF: { s32 a = (s32)R[n], b = (s32)R[m]; s32 r = (s32)((u32)a + (u32)b); c->t = (~(a ^ b) & (a ^ r)) < 0; R[n] = (u32)r; return 1; }
		}
		break;
	case 0x4:
		switch (op & 0xFF) {
		case 0x00: case 0x20: c->t = R[n] >> 31; R[n] <<= 1; return 1;
		case 0x01: c->t = R[n] & 1; R[n] >>= 1; return 1;
		case 0x04: c->t = R[n] >> 31; R[n] = (R[n] << 1) | c->t; return 1;
		case 0x05: c->t = R[n] & 1; R[n] = (R[n] >> 1) | (c->t << 31); return 1;
		case 0x08: R[n] <<= 2; return 1;
		case 0x09: R[n] >>= 2; return 1;
		case 0x10: R[n] -= 1; c->t = R[n] == 0; return 1;
		case 0x11: c->t = (s32)R[n] >= 0; return 1;
		case 0x15: c->t = (s32)R[n] > 0; return 1;
		case 0x18: R[n] <<= 8; return 1;
		case 0x19: R[n] >>= 8; return 1;
		case 0x1B: { u8 v = rd8(R[n]); c->t = v == 0; wr8(R[n], v | 0x80); return 1; }
		case 0x21: c->t = R[n] & 1; R[n] = (u32)((s32)R[n] >> 1); return 1;
		case 0x24: { u32 t = R[n] >> 31; R[n] = (R[n] << 1) | c->t; c->t = t; return 1; }
		case 0x25: { u32 t = R[n] & 1; R[n] = (R[n] >> 1) | (c->t << 31); c->t = t; return 1; }
		case 0x28: R[n] <<= 16; return 1;
		case 0x29: R[n] >>= 16; return 1;
		case 0x02: R[n] -= 4; wr32(R[n], c->mach); return 1;
		case 0x12: R[n] -= 4; wr32(R[n], c->macl); return 1;
		case 0x22: R[n] -= 4; wr32(R[n], c->pr); return 1;
		case 0x52: R[n] -= 4; wr32(R[n], c->fpul); return 1;
		case 0x62: R[n] -= 4; wr32(R[n], c->fpscr); return 1;
		case 0x03: R[n] -= 4; wr32(R[n], sr_get(c)); return 1;
		case 0x13: R[n] -= 4; wr32(R[n], c->gbr); return 1;
		case 0x23: R[n] -= 4; wr32(R[n], c->vbr); return 1;
		case 0x32: R[n] -= 4; wr32(R[n], c->sgr); return 1;
		case 0x33: R[n] -= 4; wr32(R[n], c->ssr); return 1;
		case 0x43: R[n] -= 4; wr32(R[n], c->spc); return 1;
		case 0xF2: R[n] -= 4; wr32(R[n], c->dbr); return 1;
		case 0x06: c->mach = rd32(R[n]); R[n] += 4; return 1;
		case 0x16: c->macl = rd32(R[n]); R[n] += 4; return 1;
		case 0x26: c->pr = rd32(R[n]); R[n] += 4; return 1;
		case 0x56: c->fpul = rd32(R[n]); R[n] += 4; return 1;
		case 0x66: fpscr_set(c, rd32(R[n])); R[n] += 4; return 1;
		case 0x07: sr_set(c, rd32(R[n])); R[n] += 4; return 1;
		case 0x17: c->gbr = rd32(R[n]); R[n] += 4; return 1;
		case 0x27: c->vbr = rd32(R[n]); R[n] += 4; return 1;
		case 0x37: c->ssr = rd32(R[n]); R[n] += 4; return 1;
		case 0x47: c->spc = rd32(R[n]); R[n] += 4; return 1;
		case 0xF6: c->dbr = rd32(R[n]); R[n] += 4; return 1;
		case 0x0A: c->mach = R[n]; return 1;
		case 0x1A: c->macl = R[n]; return 1;
		case 0x2A: c->pr = R[n]; return 1;
		case 0x5A: c->fpul = R[n]; return 1;
		case 0x6A: fpscr_set(c, R[n]); return 1;
		case 0x0E: sr_set(c, R[n]); return 1;
		case 0x1E: c->gbr = R[n]; return 1;
		case 0x2E: c->vbr = R[n]; return 1;
		case 0x3E: c->ssr = R[n]; return 1;
		case 0x4E: c->spc = R[n]; return 1;
		case 0xFA: c->dbr = R[n]; return 1;
		}
		if (lo == 3 && (m & 8)) { R[n] -= 4; wr32(R[n], c->r_bank[m & 7]); return 1; }
		if (lo == 7 && (m & 8)) { c->r_bank[m & 7] = rd32(R[n]); R[n] += 4; return 1; }
		if (lo == 0xE && (m & 8)) { c->r_bank[m & 7] = R[n]; return 1; }
		if (lo == 0xC) { R[n] = op_shad(R[n], R[m]); return 1; }
		if (lo == 0xD) { R[n] = op_shld(R[n], R[m]); return 1; }
		if (lo == 0xF) { op_mac_w(c, (int)n, (int)m); return 1; }
		break;
	case 0x5: R[n] = rd32(R[m] + lo * 4); return 1;
	case 0x6:
		switch (lo) {
		case 0: R[n] = (u32)(s32)(s8)rd8(R[m]); return 1;
		case 1: R[n] = (u32)(s32)(s16)rd16(R[m]); return 1;
		case 2: R[n] = rd32(R[m]); return 1;
		case 3: R[n] = R[m]; return 1;
		case 4: { u32 v = (u32)(s32)(s8)rd8(R[m]); if (n != m) R[m] += 1; R[n] = v; return 1; }
		case 5: { u32 v = (u32)(s32)(s16)rd16(R[m]); if (n != m) R[m] += 2; R[n] = v; return 1; }
		case 6: { u32 v = rd32(R[m]); if (n != m) R[m] += 4; R[n] = v; return 1; }
		case 7: R[n] = ~R[m]; return 1;
		case 8: R[n] = (R[m] & 0xFFFF0000u) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); return 1;
		case 9: R[n] = (R[m] << 16) | (R[m] >> 16); return 1;
		case 0xA: { u32 t0 = 0u - R[m]; u32 r = t0 - c->t; c->t = (0u < t0) | (t0 < r); R[n] = r; return 1; }
		case 0xB: R[n] = 0u - R[m]; return 1;
		case 0xC: R[n] = (u8)R[m]; return 1;
		case 0xD: R[n] = (u16)R[m]; return 1;
		case 0xE: R[n] = (u32)(s32)(s8)R[m]; return 1;
		case 0xF: R[n] = (u32)(s32)(s16)R[m]; return 1;
		}
		break;
	case 0x7: R[n] += (u32)s8v(imm); return 1;
	case 0x8:
		switch (n) {
		case 0x0: wr8(R[m] + lo, (u8)R[0]); return 1;
		case 0x1: wr16(R[m] + lo * 2, (u16)R[0]); return 1;
		case 0x4: R[0] = (u32)(s32)(s8)rd8(R[m] + lo); return 1;
		case 0x5: R[0] = (u32)(s32)(s16)rd16(R[m] + lo * 2); return 1;
		case 0x8: c->t = R[0] == (u32)s8v(imm); return 1;
		}
		break;
	case 0x9: R[n] = (u32)(s32)(s16)rd16(pc + 4 + imm * 2); return 1;
	case 0xC:
		switch (n) {
		case 0: wr8(c->gbr + imm, (u8)R[0]); return 1;
		case 1: wr16(c->gbr + imm * 2, (u16)R[0]); return 1;
		case 2: wr32(c->gbr + imm * 4, R[0]); return 1;
		case 4: R[0] = (u32)(s32)(s8)rd8(c->gbr + imm); return 1;
		case 5: R[0] = (u32)(s32)(s16)rd16(c->gbr + imm * 2); return 1;
		case 6: R[0] = rd32(c->gbr + imm * 4); return 1;
		case 7: R[0] = (pc & ~3u) + 4 + imm * 4; return 1;
		case 8: c->t = (R[0] & imm) == 0; return 1;
		case 9: R[0] &= imm; return 1;
		case 0xA: R[0] ^= imm; return 1;
		case 0xB: R[0] |= imm; return 1;
		case 0xC: c->t = (rd8(c->gbr + R[0]) & imm) == 0; return 1;
		case 0xD: { u32 a = c->gbr + R[0]; wr8(a, (u8)(rd8(a) & imm)); return 1; }
		case 0xE: { u32 a = c->gbr + R[0]; wr8(a, (u8)(rd8(a) ^ imm)); return 1; }
		case 0xF: { u32 a = c->gbr + R[0]; wr8(a, (u8)(rd8(a) | imm)); return 1; }
		}
		break;
	case 0xD: R[n] = rd32((pc & ~3u) + 4 + imm * 4); return 1;
	case 0xE: R[n] = (u32)s8v(imm); return 1;
	case 0xF: {
		int pr = (c->fpscr & FPSCR_PR) != 0;
		int dn = (int)(n & 14), dm = (int)(m & 14);
		float *F = c->fr.f;
		switch (lo) {
		case 0x0: if (pr) dr_set(c, dn, dr_get(c, dn) + dr_get(c, dm)); else F[n] = F[n] + F[m]; return 1;
		case 0x1: if (pr) dr_set(c, dn, dr_get(c, dn) - dr_get(c, dm)); else F[n] = F[n] - F[m]; return 1;
		case 0x2: if (pr) dr_set(c, dn, dr_get(c, dn) * dr_get(c, dm)); else F[n] = F[n] * F[m]; return 1;
		case 0x3: if (pr) dr_set(c, dn, dr_get(c, dn) / dr_get(c, dm)); else F[n] = F[n] / F[m]; return 1;
		case 0x4: c->t = pr ? dr_get(c, dn) == dr_get(c, dm) : F[n] == F[m]; return 1;
		case 0x5: c->t = pr ? dr_get(c, dn) > dr_get(c, dm) : F[n] > F[m]; return 1;
		case 0x6: fmov_load(c, (int)n, R[m] + R[0]); return 1;
		case 0x7: fmov_store(c, (int)m, R[n] + R[0]); return 1;
		case 0x8: fmov_load(c, (int)n, R[m]); return 1;
		case 0x9: fmov_load(c, (int)n, R[m]); R[m] += fmov_size(c); return 1;
		case 0xA: fmov_store(c, (int)m, R[n]); return 1;
		case 0xB: R[n] -= fmov_size(c); fmov_store(c, (int)m, R[n]); return 1;
		case 0xC: fmov_rr(c, (int)n, (int)m); return 1;
		case 0xE: F[n] = F[0] * F[m] + F[n]; return 1;
		case 0xD:
			switch (m) {
			case 0x0: c->fr.u[n] = c->fpul; return 1;
			case 0x1: c->fpul = c->fr.u[n]; return 1;
			case 0x2: if (pr) dr_set(c, dn, (double)(s32)c->fpul); else F[n] = (float)(s32)c->fpul; return 1;
			case 0x3: c->fpul = pr ? ftrc_sat(dr_get(c, dn)) : ftrc_sat(F[n]); return 1;
			case 0x4: c->fr.u[n] ^= 0x80000000u; return 1;
			case 0x5: c->fr.u[n] &= 0x7FFFFFFFu; return 1;
			case 0x6: if (pr) dr_set(c, dn, sqrt(dr_get(c, dn))); else F[n] = sqrtf(F[n]); return 1;
			case 0x7: F[n] = 1.0f / sqrtf(F[n]); return 1;
			case 0x8: c->fr.u[n] = 0; return 1;
			case 0x9: c->fr.u[n] = 0x3F800000u; return 1;
			case 0xA: { float s; memcpy(&s, &c->fpul, 4); dr_set(c, dn, s); return 1; }
			case 0xB: { float s = (float)dr_get(c, dn); memcpy(&c->fpul, &s, 4); return 1; }
			case 0xE: op_fipr(c, (int)((n >> 2) * 4), (int)((n & 3) * 4));   /* FVn = bits 11-10 gets the result */ return 1;
			case 0xF:
				if (op == 0xF3FD) { c->fpscr ^= FPSCR_SZ; return 1; }
				if (op == 0xFBFD) { fpscr_set(c, c->fpscr ^ FPSCR_FR); return 1; }
				if ((n & 1) == 0) { op_fsca(c, (int)n); return 1; }
				if ((n & 3) == 1) { op_ftrv(c, (int)((n >> 2) * 4)); return 1; }
				break;
			}
			break;
		}
		break;
	}
	}
	return 0;
}

static void call(Sh4 *c, u32 target)
{
	if (target == CORO_SAVE_ADDR) { CORO_SAVE(c); return; }
	void (*fn)(Sh4 *) = sh4_lookup(target);
	if (fn) fn(c);
	else sh4_interp(c, target);
}

void sh4_interp(Sh4 *c, u32 pc)
{
	for (;;) {
		u16 op = rd16(pc);
		interp_instructions++;
		u32 n = (op >> 8) & 15;
		u32 hi = op >> 12;
		TICK(c, 1);
		/* branches */
		if (hi == 0xA || hi == 0xB) {                    /* bra / bsr */
			u32 t = pc + 4 + (u32)(s12v(op & 0xFFF) * 2);
			u16 slot = rd16(pc + 2);
			if (!exec(c, pc + 2, slot)) goto bad_slot;
			if (hi == 0xB) { c->pr = pc + 4; call(c, t); pc += 4; }
			else {
				void (*fn)(Sh4 *) = sh4_lookup(t);
				if (fn) { fn(c); return; }                 /* tail jump into compiled code */
				pc = t;
			}
			continue;
		}
		if (hi == 0x8 && (n == 0x9 || n == 0xB || n == 0xD || n == 0xF)) {
			int taken = (n == 0x9 || n == 0xD) ? c->t : !c->t;
			u32 t = pc + 4 + (u32)(s8v(op) * 2);
			if (n == 0xD || n == 0xF) {                      /* bt/s, bf/s */
				if (!exec(c, pc + 2, rd16(pc + 2))) goto bad_slot;
				pc = taken ? t : pc + 4;
			} else {
				pc = taken ? t : pc + 2;
			}
			continue;
		}
		if ((op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x402B ||     /* jsr / jmp */
		    (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x0023) {     /* bsrf / braf */
			int is_call = (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x0003;
			int rel = (op >> 12) == 0;
			u32 t = rel ? pc + 4 + c->r[n] : c->r[n];
			if (!exec(c, pc + 2, rd16(pc + 2))) goto bad_slot;
			if (is_call) { c->pr = pc + 4; call(c, t); pc += 4; continue; }
			if (t == CORO_RESTORE_ADDR) { void (*fn)(Sh4 *) = sh4_lookup(t); if (fn) fn(c); return; }
			void (*fn)(Sh4 *) = sh4_lookup(t);
			if (fn) { fn(c); return; }
			pc = t;
			continue;
		}
		if (op == 0x000B || op == 0x002B) {              /* rts / rte */
			if (!exec(c, pc + 2, rd16(pc + 2))) goto bad_slot;
			if (op == 0x002B) sr_set(c, c->ssr);
			return;
		}
		if (!exec(c, pc, op)) {
			sh4_unimplemented(c, pc, op);
			return;
		}
		pc += 2;
	}
bad_slot:
	fprintf(stderr, "interp: branch in delay slot near %08X\n", pc);
	sh4_unimplemented(c, pc + 2, rd16(pc + 2));
}
