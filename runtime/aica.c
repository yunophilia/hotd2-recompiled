/* AICA sound chip model, see aica.h. Written from the AICA register layout:
 * 64 channels x 0x80 bytes of 16-bit registers, common registers from 0x2800,
 * ARM interrupt controller at 0x289C-0x28B0 and 0x2D00/0x2D04, ARM reset at 0x2C00. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "aica.h"
#include "arm7.h"

extern u8 aica_ram[0x800000];

int aica_enabled = 1;
s16 aica_ring[AICA_RING * 2];
volatile u32 aica_ring_write;

u16 aica_regs16[0x8000];             /* 16-bit view of 0x0000-0xFFFF */
#define REG(off) aica_regs16[((off) & 0xFFFF) >> 1]

static int arm_running;
static u64 sample_count;
static u64 last_cycles;
static double sample_acc;

/* ---------------- interrupts to the ARM ---------------- */

static u32 scieb, scipd;
static int fiq_level;

static int source_level(int bit)
{
	int b = bit > 7 ? 7 : bit;
	u16 l0 = REG(0x28A8), l1 = REG(0x28AC), l2 = REG(0x28B0);
	return ((l0 >> b) & 1) | ((l1 >> b) & 1) << 1 | ((l2 >> b) & 1) << 2;
}

static void update_fiq(void)
{
	u32 pend = scipd & scieb;
	if (!pend) { arm7.fiq_line = 0; return; }
	int bit = __builtin_ctz(pend);
	fiq_level = source_level(bit);
	arm7.fiq_line = 1;
}

static void raise(int bit)
{
	scipd |= 1u << bit;
	update_fiq();
}

/* ---------------- channels ---------------- */

typedef struct {
	int on;              /* playing */
	u32 pos;             /* sample index from SA */
	u32 frac;            /* 10.22 fixed point fraction */
	int eg_state;        /* 0 attack, 1 decay1, 2 decay2, 3 release */
	int eg;              /* attenuation 0 (loud) .. 0x3FF (silent) */
	double egf;          /* the same, with fraction */
	int eg_acc;
	int adpcm_sample, adpcm_step;
	int loop_sample, loop_step, loop_saved;
	int lp;              /* loop end reached (monitor flag) */
	int last, prev;      /* the two most recent decoded samples (interpolated by frac) */
} Chan;

static Chan ch[64];
double aica_dbg_sumsq[64], aica_dbg_gain[64];   /* debug: per-voice sample energy and mean gain */
u64 aica_dbg_n[64];

#define CR(c, o) REG((c) * 0x80 + (o))

static u32 chan_sa(int c) { return ((u32)(CR(c, 0x00) & 0x7F) << 16) | CR(c, 0x04); }
static int chan_pcms(int c) { return (CR(c, 0x00) >> 7) & 3; }

static void key_on(int c)
{
	Chan *k = &ch[c];
	k->on = 1;
	k->pos = 0;
	k->frac = 0;
	k->eg_state = 0;
	k->eg = 0x3FF;
	k->egf = 1023;
	k->eg_acc = 0;
	k->adpcm_sample = 0;
	k->adpcm_step = 127;
	k->loop_saved = 0;
	k->lp = 0;
	k->last = k->prev = 0;
}

static void key_off(int c)
{
	if (ch[c].on) ch[c].eg_state = 3;
}

/* KYONEX (bit 15 of the first register of any channel) applies KYONB of all channels */
static void key_execute(void)
{
	for (int c = 0; c < 64; c++) {
		int want = (CR(c, 0x00) >> 14) & 1;
		if (want && (!ch[c].on || ch[c].eg_state == 3)) key_on(c);
		else if (!want && ch[c].on) key_off(c);
	}
}

static const int adpcm_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };

static int adpcm_decode(Chan *k, int nibble)
{
	int mag = nibble & 7;
	int diff = ((1 + mag * 2) * k->adpcm_step) >> 3;
	int s = k->adpcm_sample + ((nibble & 8) ? -diff : diff);
	if (s > 32767) s = 32767;
	if (s < -32768) s = -32768;
	k->adpcm_sample = s;
	int st = (k->adpcm_step * adpcm_scale[mag]) >> 8;
	if (st < 127) st = 127;
	if (st > 24576) st = 24576;
	k->adpcm_step = st;
	return s;
}

static int fetch(int c, Chan *k, u32 index)
{
	u32 sa = chan_sa(c);
	switch (chan_pcms(c)) {
	case 0: { u32 a = (sa + index * 2) & 0x7FFFFE; return (s16)(aica_ram[a] | aica_ram[a + 1] << 8); }
	case 1: return (s8)aica_ram[(sa + index) & 0x7FFFFF] << 8;
	default: {
		u8 b = aica_ram[(sa + index / 2) & 0x7FFFFF];
		return adpcm_decode(k, (index & 1) ? b >> 4 : b & 15);
	}
	}
}

/* Envelope timing: milliseconds for a full attack, and for a full 0..0x3FF decay or
 * release sweep, per effective rate 0..63 (rates 0-1 never move). */
static const double attack_ms[64] = {
	-1, -1, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500,
	1200, 1000, 860, 760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95,
	76, 63, 55, 47, 38, 31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
	2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0, 0 };
static const double decay_ms[64] = {
	-1, -1, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700,
	14800, 12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100,
	920, 790, 690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49, 43, 34,
	28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1 };

/* effective rate 0..63 from a 5-bit rate and key rate scaling */
static int eg_rate(int c, int rate)
{
	if (rate == 0) return 0;
	int krs = (CR(c, 0x14) >> 10) & 15;
	int r = rate * 2;
	if (krs != 15) {
		int oct = (CR(c, 0x18) >> 11) & 15;
		if (oct & 8) oct -= 16;
		r += krs + oct * 2 + ((CR(c, 0x18) >> 9) & 1);
	}
	return r < 0 ? 0 : r > 63 ? 63 : r;
}

static void envelope(int c, Chan *k)
{
	int ar = CR(c, 0x10) & 31, d1r = (CR(c, 0x10) >> 6) & 31, d2r = (CR(c, 0x10) >> 11) & 31;
	int rr = CR(c, 0x14) & 31, dl = (CR(c, 0x14) >> 5) & 31;
	double *e = &k->egf;
	if (k->eg_state == 0) {
		double t = attack_ms[eg_rate(c, ar)];
		if (t == 0) *e = 0;
		else if (t > 0) *e = *e * pow(640.0, -1.0 / (44.1 * t)) - 1.0 / 64;   /* exponential approach */
		if (*e <= 0) { *e = 0; k->eg_state = 1; }
	} else {
		int rate = k->eg_state == 1 ? d1r : k->eg_state == 2 ? d2r : rr;
		double t = decay_ms[eg_rate(c, rate)];
		if (t == 0) *e = 1024;
		else if (t > 0) *e += 1024.0 / (44.1 * t);                                  /* linear */
		if (k->eg_state == 1 && *e >= dl << 5) k->eg_state = 2;
	}
	if (*e >= 1023) {
		*e = 1023;
		if (k->eg_state == 3) k->on = 0;
	}
	k->eg = (int)*e;
}

static void voice(int c, Chan *k, int *out_l, int *out_r)
{
	envelope(c, k);
	if (!k->on) return;
	u32 lsa = CR(c, 0x08), lea = CR(c, 0x0C);
	int loop = (CR(c, 0x00) >> 9) & 1;
	int pcms = chan_pcms(c);
	/* ADPCM must decode every sample in order; PCM can index directly */
	int s = k->last;
	int oct = (CR(c, 0x18) >> 11) & 15;
	if (oct & 8) oct -= 16;
	u32 fns = CR(c, 0x18) & 0x3FF;
	/* samples per output sample = 2^oct * (1 + fns/1024), in 10.22 fixed point */
	double inc = ldexp(1.0 + fns / 1024.0, oct);
	u64 adv = (u64)k->frac + (u64)(inc * (1 << 22));
	u32 steps = (u32)(adv >> 22);
	k->frac = (u32)(adv & ((1u << 22) - 1));
	for (u32 i = 0; i < steps; i++) {
		if (pcms >= 2 && k->pos == lsa && !k->loop_saved) {
			k->loop_sample = k->adpcm_sample; k->loop_step = k->adpcm_step; k->loop_saved = 1;
		}
		k->prev = s;
		s = fetch(c, k, k->pos);
		k->pos++;
		if (k->pos >= lea && lea) {
			k->lp = 1;
			if (loop) {
				k->pos = lsa;
				if (pcms >= 2 && k->loop_saved) { k->adpcm_sample = k->loop_sample; k->adpcm_step = k->loop_step; }
			} else {
				k->on = 0;
				k->pos = 0;
				k->eg_state = 3;
				k->eg = 0x3FF;
				k->egf = 1023;
				break;
			}
		}
	}
	k->last = s;
	/* linear interpolation between the last two samples by the position fraction
	 * (one sample behind the true position, which keeps ADPCM decoding in order) */
	s = k->prev + (int)(((s64)(s - k->prev) * k->frac) >> 22);
	/* Attenuation in 0.375 dB units: TL + envelope (10-bit EG / 4) + direct send level
	 * (DISDL: 3 dB per step, 0 = mute) + pan on one side (DIPAN: 3 dB per step,
	 * 15 = mute). 255 or more is silent. Output = sample * 2^(-att/16). */
	static const int send_level[16] = { 255, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0 };
	int voff = (CR(c, 0x28) >> 6) & 1;
	int base = voff ? 0 : (CR(c, 0x28) >> 8) + (k->eg >> 2);
	int dipan = CR(c, 0x24) & 31;
	int disdl = (CR(c, 0x24) >> 8) & 15;
	if (!disdl) {
		/* Voice goes only to the effects DSP (not emulated yet): play its DSP send
		 * (IMXL) directly instead, centred, so music on the reverb bus is not lost. */
		int imxl = (CR(c, 0x20) >> 4) & 15;
		int att = base + send_level[imxl];
		if (att < 255) {
			int v = (int)(s * pow(2.0, -att / 16.0));
			*out_l += v;
			*out_r += v;
		}
		return;
	}
	int full = base + send_level[disdl];
	aica_dbg_sumsq[c] += (double)s * s;
	aica_dbg_gain[c] += pow(2.0, -full / 16.0);
	aica_dbg_n[c]++;
	int panned = full + send_level[15 - (dipan & 15)];
	int att_l = (dipan & 16) ? full : panned;      /* bit 4 set: the right side is attenuated */
	int att_r = (dipan & 16) ? panned : full;
	if (att_l < 255) *out_l += (int)(s * pow(2.0, -att_l / 16.0));
	if (att_r < 255) *out_r += (int)(s * pow(2.0, -att_r / 16.0));
}

/* ---------------- timers ---------------- */

static int timer_div[3];

static void timers_tick(void)
{
	static const u32 reg_off[3] = { 0x2890, 0x2894, 0x2898 };
	for (int t = 0; t < 3; t++) {
		u16 r = REG(reg_off[t]);
		int pre = (r >> 8) & 7;
		if (++timer_div[t] < (1 << pre)) continue;
		timer_div[t] = 0;
		u16 count = (u16)((r & 0xFF) + 1);
		if (count > 0xFF) {
			count = 0;
			raise(6 + t);                       /* timer A/B/C -> SCIPD bits 6/7/8 */
			REG(0x28B8) |= (u16)(1u << (6 + t));  /* and the SH-4 side pending bits */
		}
		REG(reg_off[t]) = (u16)((r & 0xFF00) | (count & 0xFF));
	}
}

/* ---------------- registers ---------------- */

u32 aica_reg_read(u32 off, int size)
{
	off &= 0xFFFF;
	u32 v;
	switch (off & ~3u) {
	case 0x2810: {                                       /* channel monitor: LP, SGC, EG of MSLC */
		int c = (REG(0x280C) >> 8) & 63;
		int eg = (!ch[c].on || ch[c].eg > 0x3BF) ? 0x1FFF : ch[c].eg;   /* silent voices report 0x1FFF */
		v = (u32)(ch[c].lp << 15) | (u32)((ch[c].eg_state & 3) << 13) | (u32)eg;
		if (size == 4 || (off & 1)) ch[c].lp = 0;                          /* reading clears LP */
		break;
	}
	case 0x2814: v = ch[(REG(0x280C) >> 8) & 63].pos & 0xFFFF; break;  /* CA */
	case 0x28A0: v = scipd; break;
	case 0x289C: v = scieb; break;
	case 0x2D00: v = (u32)fiq_level; break;
	case 0x2C00: v = REG(0x2C00); break;
	default: v = REG(off & ~1u); break;
	}
	if (size == 1) v = (off & 1) ? (v >> 8) & 0xFF : v & 0xFF;
	return v;
}

void aica_reg_write(u32 off, u32 value, int size)
{
	off &= 0xFFFF;
	u16 old = REG(off & ~1u);
	u16 v;
	if (size == 1) v = (off & 1) ? (u16)((old & 0x00FF) | (value & 0xFF) << 8) : (u16)((old & 0xFF00) | (value & 0xFF));
	else v = (u16)value;
	u32 r = off & ~1u;
	switch (r & ~3u) {
	case 0x289C: scieb = v & 0x7FF; update_fiq(); return;
	case 0x28A0: if (v & 0x20) raise(5); return;          /* software interrupt request */
	case 0x28A4: scipd &= ~(u32)v; update_fiq(); return;   /* SCIRE */
	case 0x28BC: REG(0x28B8) &= (u16)~v; return;           /* MCIRE */
	case 0x2D04: if (v & 1) update_fiq(); return;          /* INTClear: re-evaluate */
	}
	REG(r) = v;
	if (r == 0x2C00) {
		int reset = v & 1;
		if (!reset && !arm_running) { arm7_reset(); arm_running = 1; }
		if (reset) arm_running = 0;
	}
	if (r < 0x2000 && (r & 0x7F) == 0 && (v & 0x8000)) {   /* KYONEX */
		REG(r) = v & 0x7FFF;
		key_execute();
	}
}

/* ---------------- ARM memory map ---------------- */

u32 arm_watch[512];   /* debug: ARM reads per word of sound RAM 0x000-0x7FF */

u32 arm_reg_reads[0x4000], arm_reg_writes[0x4000];   /* debug: ARM accesses per register word */

u32 arm_read32(u32 a)
{
	a &= 0x00FFFFFF;
	if (a < 0x800) arm_watch[a >> 2]++;
	if (a >= 0x800000 && a < 0x810000) arm_reg_reads[(a & 0xFFFF) >> 2]++;
	if (a < 0x800000) { u32 v; memcpy(&v, &aica_ram[a & 0x7FFFFC], 4); return v; }
	return aica_reg_read(a - 0x800000, 4) & 0xFFFF;
}
u8 arm_read8(u32 a)
{
	a &= 0x00FFFFFF;
	if (a < 0x800000) return aica_ram[a];
	arm_reg_reads[(a & 0xFFFF) >> 2]++;
	return (u8)aica_reg_read(a - 0x800000, 1);
}
void arm_write32(u32 a, u32 v)
{
	a &= 0x00FFFFFF;
	if (a < 0x800000) { memcpy(&aica_ram[a & 0x7FFFFC], &v, 4); return; }
	arm_reg_writes[(a & 0xFFFF) >> 2]++;
	aica_reg_write(a - 0x800000, v, 4);
}
void arm_write8(u32 a, u8 v)
{
	a &= 0x00FFFFFF;
	if (a < 0x800000) { aica_ram[a] = v; return; }
	arm_reg_writes[(a & 0xFFFF) >> 2]++;
	aica_reg_write(a - 0x800000, v, 1);
}

/* ---------------- sample loop ---------------- */

#define ARM_PER_SAMPLE arm_per_sample
int arm_per_sample = 128;   /* ARM instructions per 44.1 kHz sample (HOTD2_ARM_IPS overrides in the harness) */

static void one_sample(void)
{
	if (arm_running)
		arm7_run(ARM_PER_SAMPLE);
	raise(10);                                   /* one-sample interval interrupt */
	timers_tick();
	int l = 0, r = 0;
	for (int c = 0; c < 64; c++)
		if (ch[c].on) voice(c, &ch[c], &l, &r);
	int mvol = REG(0x2800) & 15;
	double master = mvol ? pow(10.0, -((15 - mvol) * 3.0) / 20.0) : 0;
	l = (int)(l * master); r = (int)(r * master);
	if (l > 32767) l = 32767; if (l < -32768) l = -32768;
	if (r > 32767) r = 32767; if (r < -32768) r = -32768;
	u32 w = aica_ring_write % AICA_RING;
	aica_ring[w * 2] = (s16)l;
	aica_ring[w * 2 + 1] = (s16)r;
	__atomic_store_n(&aica_ring_write, aica_ring_write + 1, __ATOMIC_RELEASE);
	sample_count++;
}

void aica_advance(u64 sh4_cycles)
{
	if (!aica_enabled) return;
	if (sh4_cycles < last_cycles) last_cycles = sh4_cycles;
	sample_acc += (double)(sh4_cycles - last_cycles) * AICA_RATE / 200000000.0;
	last_cycles = sh4_cycles;
	while (sample_acc >= 1.0) {
		one_sample();
		sample_acc -= 1.0;
	}
}

/* debug: one line per sounding voice (harness, at HOTD2_DUMP_FRAMES) */
void aica_debug_voices(FILE *f)
{
	for (int c = 0; c < 64; c++)
		if (ch[c].on)
			fprintf(f, "  voice %2d pos %5u/%04X eg %4d state %d SA %06X\n", c, ch[c].pos, CR(c, 0x0C),
			        ch[c].eg, ch[c].eg_state, chan_sa(c));
}
