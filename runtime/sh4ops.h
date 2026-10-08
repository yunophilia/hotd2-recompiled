/* Out-of-line semantics for SH-4 instructions too long to inline in generated code. */
#pragma once
#include <math.h>
#include "sh4ctx.h"

/* pref @Rn: flushes a store queue when Rn is in 0xE0000000-0xE3FFFFFF */
void hle_store_queue(Sh4 *c, u32 addr);

static inline void op_pref(Sh4 *c, u32 a)
{
	if ((a >> 26) == 0x38)
		hle_store_queue(c, a);
}

static inline void op_div1(Sh4 *c, int n, int m)
{
	/* SH-4 manual DIV1, one case per (old Q, M) */
	u32 rm = c->r[m], old_q = c->q;
	u32 q = (c->r[n] >> 31) & 1;
	u32 r = (c->r[n] << 1) | c->t;
	u32 before = r, carry;
	if (old_q == 0 && c->m == 0) { r -= rm; carry = r > before; q = q ? !carry : carry; }
	else if (old_q == 0)         { r += rm; carry = r < before; q = q ? carry : !carry; }
	else if (c->m == 0)          { r += rm; carry = r < before; q = q ? !carry : carry; }
	else                         { r -= rm; carry = r > before; q = q ? carry : !carry; }
	c->r[n] = r;
	c->q = q;
	c->t = (q == c->m);
}

static inline void op_mac_w(Sh4 *c, int n, int m)
{
	s32 a = (s16)rd16(c->r[n]); c->r[n] += 2;
	s32 b = (s16)rd16(c->r[m]); c->r[m] += 2;
	s64 mac = ((s64)c->mach << 32) | c->macl;
	if (c->s) {
		s64 v = (s64)(s32)c->macl + (s64)a * b;
		if (v > 0x7FFFFFFF) { v = 0x7FFFFFFF; c->mach |= 1; }
		else if (v < -0x80000000LL) { v = -0x80000000LL; c->mach |= 1; }
		c->macl = (u32)v;
		return;
	}
	mac += (s64)a * b;
	c->mach = (u32)((u64)mac >> 32); c->macl = (u32)mac;
}

static inline void op_mac_l(Sh4 *c, int n, int m)
{
	s64 a = (s32)rd32(c->r[n]); c->r[n] += 4;
	s64 b = (s32)rd32(c->r[m]); c->r[m] += 4;
	s64 mac = (s64)(((u64)c->mach << 32) | c->macl) + a * b;
	if (c->s) {
		const s64 hi = 0x00007FFFFFFFFFFFLL, lo = -0x0000800000000000LL;
		if (mac > hi) mac = hi; else if (mac < lo) mac = lo;
	}
	c->mach = (u32)((u64)mac >> 32); c->macl = (u32)mac;
}

static inline u32 op_shad(u32 rn, u32 rm)
{
	s32 sh = (s32)rm;
	if (sh >= 0) return rn << (sh & 31);
	if ((sh & 31) == 0) return (s32)rn < 0 ? 0xFFFFFFFFu : 0;
	return (u32)((s32)rn >> ((~sh & 31) + 1));
}

static inline u32 op_shld(u32 rn, u32 rm)
{
	s32 sh = (s32)rm;
	if (sh >= 0) return rn << (sh & 31);
	if ((sh & 31) == 0) return 0;
	return rn >> ((~sh & 31) + 1);
}

static inline u32 ftrc_sat(double v)
{
	if (v != v) return 0x80000000u;
	if (v >= 2147483647.0) return 0x7FFFFFFFu;
	if (v <= -2147483648.0) return 0x80000000u;
	return (u32)(s32)v;
}

/* fmov with FPSCR.SZ=1: register index odd = XD bank, even = DR */
static inline u32 *fpair(Sh4 *c, int r) { return (r & 1) ? &c->xf.u[r & 14] : &c->fr.u[r & 14]; }

static inline void fmov_rr(Sh4 *c, int n, int m)
{
	if (c->fpscr & FPSCR_SZ) { u32 *d = fpair(c, n), *s = fpair(c, m); d[0] = s[0]; d[1] = s[1]; }
	else c->fr.u[n] = c->fr.u[m];
}
static inline void fmov_load(Sh4 *c, int n, u32 a)
{
	if (c->fpscr & FPSCR_SZ) { u32 *d = fpair(c, n); d[0] = rd32(a); d[1] = rd32(a + 4); }
	else c->fr.u[n] = rd32(a);
}
static inline void fmov_store(Sh4 *c, int m, u32 a)
{
	if (c->fpscr & FPSCR_SZ) { u32 *s = fpair(c, m); wr32(a, s[0]); wr32(a + 4, s[1]); }
	else wr32(a, c->fr.u[m]);
}
static inline u32 fmov_size(const Sh4 *c) { return (c->fpscr & FPSCR_SZ) ? 8 : 4; }

static inline void op_fipr(Sh4 *c, int n, int m)
{
	float *a = &c->fr.f[n], *b = &c->fr.f[m];
	c->fr.f[n + 3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static inline void op_ftrv(Sh4 *c, int n)
{
	float v[4] = { c->fr.f[n], c->fr.f[n + 1], c->fr.f[n + 2], c->fr.f[n + 3] };
	const float *x = c->xf.f;
	for (int i = 0; i < 4; i++)
		c->fr.f[n + i] = x[i] * v[0] + x[i + 4] * v[1] + x[i + 8] * v[2] + x[i + 12] * v[3];
}

static inline void op_fsca(Sh4 *c, int n)
{
	double a = (c->fpul & 0xFFFF) * (2.0 * 3.14159265358979323846 / 65536.0);
	c->fr.f[n] = (float)sin(a);
	c->fr.f[n + 1] = (float)cos(a);
}
