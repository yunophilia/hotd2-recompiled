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
#include <signal.h>
#include <unistd.h>
#include "sh4ctx.h"
#include "funcs.h"

u8 *ram;
static Sh4 cpu;
static unsigned long hw_count;
static unsigned long MAX_HW = 200000;

static void stop(const char *why)
{
	fprintf(stderr, "stop: %s (hw accesses: %lu)\n", why, hw_count);
	exit(0);
}

extern void (*hle_trace)(const char *kind, u32 addr, u32 value, int size);

/* last accesses, printed when the harness is killed (to see what a hang is polling) */
static struct { const char *kind; u32 addr, value; int size; } ring[32];
static unsigned ring_pos;

static void dump_ring(int sig)
{
	(void)sig;
	fprintf(stderr, "--- last hardware accesses (of %lu) ---\n", hw_count);
	for (unsigned i = 0; i < 32; i++) {
		unsigned k = (ring_pos + i) % 32;
		if (ring[k].kind)
			fprintf(stderr, "  %s%d %08X = %08X\n", ring[k].kind, ring[k].size * 8, ring[k].addr, ring[k].value);
	}
	_exit(2);
}

/* Print each distinct (kind, register) the first time it is touched. */
static void hw_trace(const char *kind, u32 addr, u32 value, int size)
{
	static u32 seen[4096];
	static int nseen;
	u32 key = (addr & 0x1FFFFFFC) ^ (kind[0] == 'W' ? 0x80000000u : 0) ^ (kind[0] == 'S' ? 0x40000000u : 0);
	int fresh = 1;
	for (int i = 0; i < nseen; i++)
		if (seen[i] == key) { fresh = 0; break; }
	if (fresh && nseen < 4096) {
		seen[nseen++] = key;
		fprintf(stderr, "  hw %s%d %08X = %08X  (#%lu)\n", kind, size * 8, addr, value, hw_count);
	}
	ring[ring_pos % 32].kind = kind; ring[ring_pos % 32].addr = addr;
	ring[ring_pos % 32].value = value; ring[ring_pos % 32].size = size; ring_pos++;
	if (++hw_count >= MAX_HW)
		stop("hardware access budget used up");
}

#ifdef TRACE_LOWRAM
void trace_lowram(u32 a, int write, int size)
{
	static u8 seen[0x20000 / 4];
	u32 o = (a & 0x1FFFF) / 4;
	(void)write; (void)size;
	if (!seen[o]) {
		seen[o] = 1;
		u32 v; memcpy(&v, ram + (a & 0x1FFFC), 4);
		fprintf(stderr, "  lowram %08X (now %08X)\n", a, v);
	}
}
#endif

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
	FILE *m = fopen("missing.txt", "a");
	if (m) { fprintf(m, "%08X  # from pr=%08X\n", target, c->pr); fclose(m); }
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
		load(argv[1], ram + 0x20000, 0x125E10); /* code + .data, up to BSS */
	u32 entry = argc > 3 ? (u32)strtoul(argv[3], NULL, 0) : 0x0C020000u;
	cpu.sr_rest = 0x40000000u; /* MD=1 */
	cpu.fpscr = 0x00040001u;   /* SH-4 reset value */
	cpu.r[15] = 0x0CA553C0u;
	extern void hle_init(Sh4 *c);
	extern int cart_load(const char *path);
	const char *cart = getenv("HOTD2_CART") ? getenv("HOTD2_CART") : "cart.bin";
	if (!cart_load(cart))
		fprintf(stderr, "warning: no cart image (%s); cart reads return 0xFF\n", cart);
	hle_init(&cpu);
	hle_trace = hw_trace;
	signal(SIGTERM, dump_ring);
	signal(SIGINT, dump_ring);
	fprintf(stderr, "running from %08X\n", entry);
	sh4_dispatch(&cpu, entry);
	stop("entry function returned");
}
