/* Native test harness for recompiled code.
 *
 *   harness <hotd2_ic22.bin> [ram_dump.bin] [entry]
 *
 * Loads the program image (or a full RAM dump) at 0x0C000000, then runs from
 * entry (default: crt0). Hardware accesses are logged, not emulated; the run
 * stops at the first unimplemented instruction or after MAX_HW accesses.
 */
#include <stdio.h>
#include <stdlib.h>
#include "sh4ctx.h"
#include "funcs.h"

u8 *ram;
static Sh4 cpu;
static unsigned long hw_count;
#define MAX_HW 2000

static void stop(const char *why)
{
	fprintf(stderr, "stop: %s (hw accesses: %lu)\n", why, hw_count);
	exit(0);
}

static void hw_log(const char *dir, u32 addr, u32 value, int size)
{
	if (hw_count < 200)
		fprintf(stderr, "  hw %s%d %08X = %08X\n", dir, size * 8, addr, value);
	if (++hw_count >= MAX_HW)
		stop("hardware access budget used up");
}

u32 hle_read(u32 addr, int size)
{
	hw_log("R", addr, 0, size);
	return 0;
}

void hle_write(u32 addr, u32 value, int size)
{
	hw_log("W", addr, value, size);
}

void hle_store_queue(Sh4 *c, u32 addr)
{
	(void)c;
	hw_log("SQ", addr, 0, 32);
}

void sh4_dispatch(Sh4 *c, u32 target)
{
	u32 lo = 0, hi = func_count;
	target &= 0x1FFFFFFF;
	target |= 0x0C000000u & ~0x1FFFFFFFu; /* keep it in P0 for the lookup */
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		u32 a = func_table[mid].addr & 0x1FFFFFFF;
		if (a == target) { func_table[mid].fn(c); return; }
		if (a < target) lo = mid + 1; else hi = mid;
	}
	fprintf(stderr, "dispatch: no function at %08X (pr=%08X)\n", target, c->pr);
	stop("unknown dispatch target");
}

void sh4_unimplemented(Sh4 *c, u32 pc, u16 op)
{
	fprintf(stderr, "unimplemented %04X at %08X (r15=%08X pr=%08X)\n", op, pc, c->r[15], c->pr);
	stop("unimplemented");
}

static size_t load(const char *path, u8 *dst, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	size_t n = fread(dst, 1, max, f);
	fclose(f);
	return n;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s program.bin [ram_dump.bin] [entry]\n", argv[0]);
		return 1;
	}
	ram = calloc(1, RAM_SIZE);
	if (argc > 2 && argv[2][0])
		load(argv[2], ram, RAM_SIZE);
	else
		load(argv[1], ram + 0x20000, 0x123000);
	u32 entry = argc > 3 ? (u32)strtoul(argv[3], NULL, 0) : 0x0C020000u;
	cpu.sr_rest = 0x40000000u; /* MD=1 */
	cpu.fpscr = 0x00040001u;   /* SH-4 reset value */
	cpu.r[15] = 0x0CA553C0u;
	fprintf(stderr, "running from %08X\n", entry);
	sh4_dispatch(&cpu, entry);
	stop("entry function returned");
}
