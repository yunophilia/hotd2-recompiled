/* SH-4 guest state and memory access for recompiled HOTD2 code.
 *
 * Every recompiled function has the signature `void fn(Sh4 *c)`. Guest code
 * and its stack live in `ram` (32 MB at 0x0C000000, mirrored through P0-P3);
 * anything else goes to the HLE runtime through hle_read / hle_write.
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define RAM_SIZE 0x02000000u
#define RAM_MASK (RAM_SIZE - 1)

/* FPSCR bits the generated code checks at run time */
#define FPSCR_PR (1u << 19) /* double precision */
#define FPSCR_SZ (1u << 20) /* 64-bit fmov */
#define FPSCR_FR (1u << 21) /* bank select (handled by swapping fr/xf) */

typedef struct Sh4 {
	u32 r[16];
	u32 r_bank[8];
	union { float f[16]; u32 u[16]; } fr; /* current bank, fr0..fr15 */
	union { float f[16]; u32 u[16]; } xf; /* other bank */
	u32 t, s, q, m;          /* SR flags kept unpacked */
	u32 sr_rest;             /* remaining SR bits (MD, RB, BL, IMASK) */
	u32 gbr, vbr, ssr, spc, sgr, dbr;
	u32 mach, macl, pr;
	u32 fpscr, fpul;
	u32 pc;                  /* only meaningful at dispatch boundaries */
	u64 cycles;              /* approximate, drives HLE timers / vblank */
} Sh4;

extern u8 *ram;

/* HLE runtime: everything that is not main RAM. */
u32 hle_read(u32 addr, int size);
void hle_write(u32 addr, u32 value, int size);
/* Called for calls/jumps whose target is only known at run time. */
void sh4_dispatch(Sh4 *c, u32 target);
/* Unrecompiled or unexpected code paths end up here. */
void sh4_unimplemented(Sh4 *c, u32 pc, u16 op);

static inline int is_ram(u32 a) { return (a & 0x1F000000u) == 0x0C000000u; }

static inline u8 rd8(u32 a) { return is_ram(a) ? ram[a & RAM_MASK] : (u8)hle_read(a, 1); }
static inline u16 rd16(u32 a)
{
	if (is_ram(a)) { u16 v; memcpy(&v, ram + (a & RAM_MASK), 2); return v; }
	return (u16)hle_read(a, 2);
}
static inline u32 rd32(u32 a)
{
	if (is_ram(a)) { u32 v; memcpy(&v, ram + (a & RAM_MASK), 4); return v; }
	return hle_read(a, 4);
}
static inline void wr8(u32 a, u8 v) { if (is_ram(a)) ram[a & RAM_MASK] = v; else hle_write(a, v, 1); }
static inline void wr16(u32 a, u16 v)
{
	if (is_ram(a)) memcpy(ram + (a & RAM_MASK), &v, 2); else hle_write(a, v, 2);
}
static inline void wr32(u32 a, u32 v)
{
	if (is_ram(a)) memcpy(ram + (a & RAM_MASK), &v, 4); else hle_write(a, v, 4);
}

static inline float rdf(u32 a) { u32 v = rd32(a); float f; memcpy(&f, &v, 4); return f; }
static inline void wrf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); wr32(a, v); }

/* SR as the guest sees it (stc sr / ldc sr) */
static inline u32 sr_get(const Sh4 *c)
{
	return c->sr_rest | (c->m << 9) | (c->q << 8) | (c->s << 1) | c->t;
}
static inline void sr_set(Sh4 *c, u32 v)
{
	c->t = v & 1; c->s = (v >> 1) & 1; c->q = (v >> 8) & 1; c->m = (v >> 9) & 1;
	c->sr_rest = v & ~0x303u;
}

static inline void fpscr_set(Sh4 *c, u32 v)
{
	if ((v ^ c->fpscr) & FPSCR_FR) {
		for (int i = 0; i < 16; i++) { u32 t = c->fr.u[i]; c->fr.u[i] = c->xf.u[i]; c->xf.u[i] = t; }
	}
	c->fpscr = v & 0x003FFFFFu;
}

/* Double-precision register pair DRn (n even): fr[n] holds the high word. */
static inline double dr_get(const Sh4 *c, int n)
{
	u64 v = ((u64)c->fr.u[n] << 32) | c->fr.u[n + 1];
	double d; memcpy(&d, &v, 8); return d;
}
static inline void dr_set(Sh4 *c, int n, double d)
{
	u64 v; memcpy(&v, &d, 8);
	c->fr.u[n] = (u32)(v >> 32); c->fr.u[n + 1] = (u32)v;
}
