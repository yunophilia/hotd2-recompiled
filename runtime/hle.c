/* Memory-mapped hardware for recompiled HOTD2: everything outside main RAM.
 *
 * Registers we do not model yet behave like plain memory (reads return the
 * last value written), and every access to one is reported through
 * hle_trace so the harness can show what the game expects.
 */
#include <stdio.h>
#include <string.h>
#include "sh4ctx.h"
#include "jvs.h"
#include "cart.h"
#include "ta.h"

void (*hle_trace)(const char *kind, u32 addr, u32 value, int size);
static Sh4 *hle_cpu;

/* Holly system-block registers 0x005F6800-0x005F7FFF */
static u32 sb[0x1800 / 4];
/* PVR registers 0x005F8000-0x005F9FFF */
static u32 pvr[0x2000 / 4];
/* SH-4 on-chip registers, 0x1F000000-0x1FFFFFFF (P4 0xFF..), sparse: low 16 bits of each area */
static u32 onchip[256][0x100 / 4];

/* PVR2 video RAM (16 MB on NAOMI), stored in 64-bit-path order. Area 1:
 * 0x04/0x06xxxxxx = 64-bit path (linear), 0x05/0x07xxxxxx = 32-bit path, where
 * consecutive words alternate between the two 8 MB banks. */
u8 vram[0x1000000];

static inline u32 vram32_to_linear(u32 o)
{
	u32 bank = (o >> 23) & 1;
	return (o & 3) | ((o & 0x7FFFFCu) << 1) | (bank << 2);
}

static inline u8 *vram_ptr(u32 p)
{
	if ((p & 0x1C000000u) != 0x04000000u)
		return NULL;
	u32 o = p & 0xFFFFFFu;
	return (p & 0x01000000u) ? &vram[vram32_to_linear(o)] : &vram[o];
}

/* SH-4 store queues: two 32-byte buffers at 0xE0000000-0xE3FFFFFF, flushed by
 * `pref` to the area selected by QACR0/QACR1 (0xFF000038/0xFF00003C). */
static u8 sq_buf[2][32];
#define QACR(n) onchip[0][(0x38 + 4 * (n)) / 4]

void hle_store_queue(Sh4 *c, u32 addr)
{
	(void)c;
	int n = (addr >> 5) & 1;
	u32 dst = (((QACR(n) >> 2) & 7) << 26) | (addr & 0x03FFFFE0u);
	if (hle_trace) hle_trace("SQ", dst, 0, 32);
	ta_write(dst, sq_buf[n], 32);
}

/* SH-4 TMU: three down-counters clocked at Pphi (50 MHz) / prescaler */
#define TMU_TSTR 0xFFD80004u
#define TMU(off) onchip[0xD8][(off) / 4]
static u64 tmu_start[3];
static u32 tmu_base[3];

static u32 tmu_count(int n)
{
	u32 tcr = TMU(0x10 + 12 * n), tcor = TMU(0x08 + 12 * n);
	if (!(TMU(0x04) & (1u << n)))
		return tmu_base[n];
	static const u32 div[8] = { 4, 16, 64, 256, 1024, 1024, 1024, 1024 };
	u64 ticks = (hle_cpu->cycles - tmu_start[n]) / 4 / div[tcr & 7];
	if (ticks <= tmu_base[n])
		return tmu_base[n] - (u32)ticks;
	u64 period = (u64)tcor + 1;
	return tcor - (u32)((ticks - tmu_base[n] - 1) % period);
}

static int tmu_read(u32 addr, u32 *v)
{
	for (int n = 0; n < 3; n++)
		if (addr == 0xFFD8000Cu + 12 * n) { *v = tmu_count(n); return 1; }
	return 0;
}

static int tmu_write(u32 addr, u32 v)
{
	for (int n = 0; n < 3; n++)
		if (addr == 0xFFD8000Cu + 12 * n) {
			tmu_base[n] = v;
			tmu_start[n] = hle_cpu->cycles;
			return 1;
		}
	if (addr == TMU_TSTR) {
		u32 old = TMU(0x04);
		for (int n = 0; n < 3; n++) {
			u32 bit = 1u << n;
			if ((v & bit) && !(old & bit)) tmu_start[n] = hle_cpu->cycles;           /* start */
			if (!(v & bit) && (old & bit)) tmu_base[n] = tmu_count(n);               /* stop: freeze */
		}
		TMU(0x04) = v;
		return 1;
	}
	return 0;
}

/* AICA sound RAM (8 MB on NAOMI) at 0x00800000, and its register block at
 * 0x00700000. The ARM7 sound CPU is not emulated yet: the RAM is plain memory. */
u8 aica_ram[0x800000];
static u32 aica_regs[0x10000 / 4];

static inline u8 *aica_ptr(u32 p)
{
	if (p >= 0x00800000u && p < 0x01000000u)
		return &aica_ram[p & 0x7FFFFFu];
	return NULL;
}

/* NAOMI battery-backed SRAM: bookkeeping, settings, high scores */
u8 naomi_sram[0x8000];
#define SRAM_BASE 0x00200000u

#define SB_MDSTAR 0x005F6C04u
#define SB_MDEN   0x005F6C14u
#define SB_MDST   0x005F6C18u

/* Holly interrupt controller */
#define SB_ISTNRM 0x005F6900u
#define SB_ISTEXT 0x005F6904u
#define SB_ISTERR 0x005F6908u
#define SB_IML2NRM 0x005F6910u /* + 0x00/0x04/0x08 for NRM/EXT/ERR, levels 2/4/6 at +0x00/+0x10/+0x20 */
static u32 istnrm, istext, isterr;

/* PVR video timing */
#define SPG_VBLANK_INT 0x005F80CCu
#define SPG_LOAD       0x005F80D8u
#define SPG_STATUS     0x005F810Cu
#define SH4_HZ 200000000ull
u64 hle_next_event = 1;
static u32 scanline;
u64 hle_frames;
u64 hle_renders;

#define SB(a) sb[((a) - 0x005F6800u) / 4]
#define PVR(a) pvr[((a) - 0x005F8000u) / 4]

void hle_raise_normal(int bit) { istnrm |= 1u << bit; }

static u32 spg_lines(void)
{
	u32 v = (PVR(SPG_LOAD) >> 16) & 0x3FF;
	return v ? v + 1 : 525;
}

static u32 holly_level(void)
{
	static const int levels[3] = { 6, 4, 2 };
	for (int k = 0; k < 3; k++) {
		u32 base = SB_IML2NRM + 0x20 - 0x10 * k;   /* IML6 first */
		if ((SB(base) & istnrm) || (SB(base + 4) & istext) || (SB(base + 8) & isterr))
			return levels[k];
	}
	return 0;
}

void sh4_dispatch(Sh4 *c, u32 target);

/* BIOS HLE for interrupts. The NAOMI BIOS leaves VBR at 0x8C000000; its stub at
 * VBR+0x600 saves the whole CPU context, switches to its own stack and calls
 * the callback the game registered at VBR + 0x1C0 + (INTEVT >> 3). Callbacks
 * are ordinary functions (they return with rts). */
#define BIOS_VBR       0x8C000000u
#define BIOS_IRQ_TABLE 0x1C0u
#define BIOS_IRQ_STACK 0x8C00FF00u   /* inside the BIOS-owned 0x0C000000-0x0C01FFFF */

static void take_interrupt(Sh4 *c, u32 level)
{
	u32 intevt = level == 6 ? 0x320 : level == 4 ? 0x360 : 0x3A0;
	onchip[0][0x28 / 4] = intevt;              /* INTEVT, 0xFF000028 */
	u32 handler = rd32(c->vbr + BIOS_IRQ_TABLE + (intevt >> 3));
	if ((handler & 0x1FFFFFFF) < 0x0C020000u || (handler & 0x1FFFFFFF) >= 0x0C143000u) {
		/* nothing registered: acknowledge what this level would have serviced */
		u32 base = SB_IML2NRM + 0x10 * ((level - 2) / 2);
		istnrm &= ~SB(base);
		istext &= ~SB(base + 4);
		isterr &= ~SB(base + 8);
		return;
	}
	Sh4 saved = *c;
	c->ssr = sr_get(c);
	c->spc = 0;
	sr_set(c, c->ssr | SR_MD | SR_BL);         /* stay blocked: no nesting */
	c->r[15] = BIOS_IRQ_STACK;
	c->r[4] = intevt;
	sh4_dispatch(c, handler);
	saved.cycles = c->cycles;
	*c = saved;
}

void hle_init(Sh4 *c)
{
	hle_cpu = c;
	c->vbr = BIOS_VBR;
}

static void next_scanline(void)
{
	u32 lines = spg_lines();
	u32 vint = PVR(SPG_VBLANK_INT);
	u32 vin = vint ? (vint & 0x3FF) : 0x208, vout = vint ? ((vint >> 16) & 0x3FF) : 0x015;
	scanline = (scanline + 1) % lines;
	if (scanline == vin) { hle_raise_normal(3); hle_frames++; }   /* vblank in */
	if (scanline == vout) hle_raise_normal(4);                    /* vblank out */
	/* hblank interrupt per SPG_HBLANK_INT: mode 0 = at the compare line,
	 * 1 = every <compare> lines, 2 = every line */
	u32 hint = PVR(0x005F80C8u), comp = hint & 0x3FF, mode = (hint >> 12) & 3;
	if ((mode == 0 && scanline == comp) || (mode == 1 && comp && scanline % comp == 0) || mode == 2)
		hle_raise_normal(5);
}

void hle_event(Sh4 *c)
{
	u64 per_line = SH4_HZ / 60 / spg_lines();
	while (c->cycles >= hle_next_event) {
		next_scanline();
		hle_next_event += per_line;
	}
	u32 level = holly_level();
	u32 imask = (sr_get(c) >> 4) & 0xF;
	if (level && level > imask && !(c->sr_rest & SR_BL))
		take_interrupt(c, level);
}

static u32 *reg_slot(u32 addr, int *known)
{
	u32 p = addr & 0x1FFFFFFF;
	*known = 0;
	if (addr >= 0xFF000000u || (p >= 0x1F000000u)) {
		u32 area = (addr >> 16) & 0xFF;   /* FF00, FF80, FFA0, FFD8... */
		return &onchip[area][(addr & 0xFF) / 4];
	}
	if (p >= 0x005F6800u && p < 0x005F8000u)
		return &sb[(p - 0x005F6800u) / 4];
	if (p >= 0x005F8000u && p < 0x005FA000u)
		return &pvr[(p - 0x005F8000u) / 4];
	return NULL;
}

u32 hle_read(u32 addr, int size)
{
	u32 p = addr & 0x1FFFFFFF;
	int known;
	u32 v = 0;
	if ((addr >> 26) == 0x38) {                 /* store-queue area reads back the buffer */
		memcpy(&v, &sq_buf[(addr >> 5) & 1][addr & 0x1C], 4);
		return v;
	}
	u8 *vp = vram_ptr(p);
	if (!vp) vp = aica_ptr(p);
	if (vp) {
		memcpy(&v, vp, size);
		return v;
	}
	if (p >= 0x00700000u && p < 0x00710000u) {
		v = aica_regs[(p & 0xFFFF) / 4];
		if (hle_trace) hle_trace("R", addr, v, size);
		return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
	}
	if (cart_read_reg(p, &v) || tmu_read(addr, &v)) {
		/* handled */
	} else if (p == SB_MDST) {
		v = 0; /* DMA finishes synchronously */
	} else if (p == SB_ISTNRM) {
		v = istnrm | (isterr ? 1u << 31 : 0) | (istext ? 1u << 30 : 0);
	} else if (p == SB_ISTEXT) {
		v = istext;
	} else if (p == SB_ISTERR) {
		v = isterr;
	} else if (p == 0x005F8000u) {
		v = 0x17FD11DBu;   /* PVR2 (CLX2) chip ID */
	} else if (p == 0x005F8004u) {
		v = 0x00000011u;   /* revision */
	} else if (p == SPG_STATUS) {
		u32 vint = PVR(SPG_VBLANK_INT);
		u32 vin = vint ? (vint & 0x3FF) : 0x208, vout = vint ? ((vint >> 16) & 0x3FF) : 0x015;
		int vsync = scanline >= vin || scanline < vout;
		v = scanline | (vsync ? 1u << 13 : 0);
	} else if (p - SRAM_BASE < sizeof(naomi_sram)) {
		memcpy(&v, naomi_sram + (p - SRAM_BASE), size);
		return v; /* plain memory, not worth tracing */
	} else {
		u32 *slot = reg_slot(addr, &known);
		if (slot)
			v = *slot;
	}
	if (size == 1) v &= 0xFF;
	else if (size == 2) v &= 0xFFFF;
	if (hle_trace) hle_trace("R", addr, v, size);
	return v;
}

/* PVR channel-2 DMA: main RAM (source in DMAC SAR2) -> TA / texture memory */
static void ch2_dma(void)
{
	u32 dst = SB(0x005F6800u), len = SB(0x005F6804u);
	u32 *sar2 = &onchip[0xA0][0x20 / 4], *dmatcr2 = &onchip[0xA0][0x28 / 4], *chcr2 = &onchip[0xA0][0x2C / 4];
	u8 chunk[32];
	for (u32 k = 0; k < len; k += 32) {
		for (int b = 0; b < 32; b++) chunk[b] = rd8(*sar2 + k + b);
		ta_write(dst + ((dst & 0x1FFFFFFF) >= 0x11000000u ? k : 0), chunk, 32);
	}
	*sar2 += len;
	*dmatcr2 = 0;
	*chcr2 |= 2;                   /* TE: transfer end */
	SB(0x005F6800u) = dst + len;
	SB(0x005F6804u) = 0;
	SB(0x005F6808u) = 0;
	hle_raise_normal(19);          /* ch2 DMA end */
}

/* G1 (GD/cart) DMA: cart ROM -> main RAM */
static void g1_dma(void)
{
	u32 dst = SB(0x005F7404u), len = SB(0x005F7408u);
	cart_dma(0x0C000000u | (dst & 0x01FFFFFFu), len);
	SB(0x005F74F4u) = dst + len;  /* GDSTARD */
	SB(0x005F74F8u) = len;        /* GDLEND */
	SB(0x005F7418u) = 0;
	hle_raise_normal(14);         /* G1 DMA end */
}

void hle_write(u32 addr, u32 value, int size)
{
	u32 p = addr & 0x1FFFFFFF;
	int known;
	if ((addr >> 26) == 0x38) {                 /* store-queue buffer */
		memcpy(&sq_buf[(addr >> 5) & 1][addr & 0x1F], &value, size);
		return;
	}
	if (p - SRAM_BASE < sizeof(naomi_sram)) {
		memcpy(naomi_sram + (p - SRAM_BASE), &value, size);
		return;
	}
	u8 *vp = vram_ptr(p);
	if (!vp) vp = aica_ptr(p);
	if (vp) {
		memcpy(vp, &value, size);
		return;
	}
	if (hle_trace) hle_trace("W", addr, value, size);
	if (p >= 0x00700000u && p < 0x00710000u) {
		u32 *r = &aica_regs[(p & 0xFFFF) / 4];
		/* STUB until the ARM7 sound CPU is emulated: when the game releases it from
		 * reset, pretend the uploaded driver started and posted its "alive" word at
		 * sound RAM 0x5C, which the game polls during boot (f_0c0ba70c). */
		if (p == 0x00702C00u && (*r & 1) && !(value & 1)) {
			u32 alive = 1;
			memcpy(&aica_ram[0x5C], &alive, 4);
		}
		*r = value;
		return;
	}
	if (cart_write_reg(p, value) || tmu_write(addr, value))
		return;
	if (p == SB_ISTNRM) { istnrm &= ~(value & 0x3FFFFFFFu); return; }  /* write 1 to clear */
	if (p == SB_ISTERR) { isterr &= ~value; return; }
	u32 *slot = reg_slot(addr, &known);
	if (slot) {
		if (size == 4) *slot = value;
		else if (size == 2) *slot = (*slot & ~0xFFFFu) | (value & 0xFFFF);
		else *slot = (*slot & ~0xFFu) | (value & 0xFF);
	}
	if (p == SB_MDST && (value & 1) && (SB(SB_MDEN) & 1)) {
		maple_dma(SB(SB_MDSTAR));
		hle_raise_normal(12); /* Maple DMA end */
	}
	if (p == 0x005F6808u && (value & 1))
		ch2_dma();
	if (p == 0x005F8144u && (value & 0x80000000u))   /* TA_LIST_INIT */
		ta_list_reset();
	if (p == 0x005F8014u) {                         /* STARTRENDER: not drawn yet, report done */
		extern u64 hle_renders;
		hle_renders++;
		hle_raise_normal(0); hle_raise_normal(1); hle_raise_normal(2);
	}
	if (p == 0x005F7418u && (value & 1) && (SB(0x005F7414u) & 1))
		g1_dma();
}
