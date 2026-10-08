/* Differential check of the recompiler against the interpreter (native
 * harness only, built with -DHOTD2_DIFF).
 *
 * Every recompiled function starts with DIFF_ENTER. The first few times a
 * function is called, we snapshot the guest (registers and all of RAM), run
 * the interpreter on it, restore, run the recompiled code, and compare the two
 * results. Then we restore once more and let the call run for real. Callees run
 * recompiled in both trials, so each check covers one function body.
 *
 * Trials run with HLE events held off. A trial that touches hardware (any
 * hle_read/hle_write) or the coroutine machinery cannot be repeated safely, so
 * it is abandoned before the access and the function is marked untestable, as is one that runs
 * longer than TRIAL_CYCLES (main loops, waits for vblank).
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh4ctx.h"

void sh4_interp(Sh4 *c, u32 pc);
extern u64 hle_next_event;

int diff_trial;              /* inside a trial: hardware access, coroutine switches and
                              * hle_event (the cycle budget) longjmp to diff_abort */
jmp_buf diff_abort;

#define TRIAL_CYCLES 20000000u

static int busy;             /* inside a trial: no nested checks */
static int pass_through;     /* the next entry is the real call of a checked function */
static u8 *snap, *res;
static u32 tests, fails, untestable, checked;

#define SLOTS 16384
static struct { u32 addr; u8 runs; u8 state; } seen[SLOTS];   /* state: 0 testing, 1 failed, 2 untestable */

static int slot(u32 a)
{
	u32 h = (a * 2654435761u) % SLOTS;
	while (seen[h].addr && seen[h].addr != a) h = (h + 1) % SLOTS;
	seen[h].addr = a;
	return (int)h;
}

static const char *reg_name(int w, char *buf)
{
	static const struct { int at; const char *n; } f[] = {
		{0, "r"}, {16, "r_bank"}, {24, "fr"}, {40, "xf"}, {56, "t"}, {57, "s"}, {58, "q"}, {59, "m"},
		{60, "sr_rest"}, {61, "gbr"}, {62, "vbr"}, {63, "ssr"}, {64, "spc"}, {65, "sgr"}, {66, "dbr"},
		{67, "mach"}, {68, "macl"}, {69, "pr"}, {70, "fpscr"}, {71, "fpul"}, {72, "pc"},
	};
	int k = 0;
	for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) if (f[i].at <= w) k = (int)i;
	sprintf(buf, "%s[%d]", f[k].n, w - f[k].at);
	return buf;
}

static void report(u32 addr, const Sh4 *ic, const Sh4 *cc)
{
	char nb[32];
	fprintf(stderr, "diff: %08X differs:", addr);
	const u32 *a = (const u32 *)ic, *b = (const u32 *)cc;
	int shown = 0;
	for (int w = 0; w < 72; w++)     /* up to (not including) pc and cycles */
		if (a[w] != b[w] && shown++ < 8)
			fprintf(stderr, " %s interp=%08X recomp=%08X", reg_name(w, nb), a[w], b[w]);
	shown = 0;
	for (u32 o = 0; o < RAM_SIZE; o += 4)
		if (memcmp(res + o, ram + o, 4) && shown++ < 8) {
			u32 x, y;
			memcpy(&x, res + o, 4); memcpy(&y, ram + o, 4);
			fprintf(stderr, " [%08X] interp=%08X recomp=%08X", 0x0C000000u + o, x, y);
		}
	fprintf(stderr, "\n");
}

int diff_enter(Sh4 *c, u32 addr, void (*fn)(Sh4 *))
{
	if (pass_through) { pass_through = 0; return 0; }
	if (busy) return 0;
	int h = slot(addr & 0x1FFFFFFFu);
	if (seen[h].state || seen[h].runs >= 3) return 0;
	seen[h].runs++;
	if (!snap) { snap = malloc(RAM_SIZE); res = malloc(RAM_SIZE); }
	busy = 1;
	Sh4 s0 = *c, ic;
	memcpy(snap, ram, RAM_SIZE);
	u64 ev = hle_next_event;
	hle_next_event = c->cycles + TRIAL_CYCLES;   /* hle_event aborts a trial that runs this long */
	diff_trial = 1;
	int ok = !setjmp(diff_abort);
	if (ok) { sh4_interp(c, addr); ic = *c; memcpy(res, ram, RAM_SIZE); }
	if (ok) {
		*c = s0;
		memcpy(ram, snap, RAM_SIZE);
		hle_next_event = c->cycles + TRIAL_CYCLES;
		ok = !setjmp(diff_abort);
		if (ok) fn(c);
	}
	diff_trial = 0;
	hle_next_event = ev;
	if (!ok) {
		seen[h].state = 2;
		untestable++;
	} else {
		if (seen[h].runs == 1) checked++;
		tests++;
		Sh4 cc = *c;
		ic.pc = cc.pc; ic.cycles = cc.cycles;
		if (memcmp(&ic, &cc, sizeof ic) || memcmp(res, ram, RAM_SIZE)) {
			fails++;
			seen[h].state = 1;
			report(addr, &ic, &cc);
		}
	}
	*c = s0;
	memcpy(ram, snap, RAM_SIZE);
	busy = 0;
	pass_through = 1;
	fn(c);                       /* the real call (its callees may be checked) */
	return 1;
}

void diff_summary(void)
{
	fprintf(stderr, "--- diff: %u checks of %u functions, %u functions differ, %u untestable ---\n",
		tests, checked, fails, untestable);
}
