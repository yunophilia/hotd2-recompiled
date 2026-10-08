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
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "sh4ctx.h"
#include "funcs.h"

u8 *ram;
static Sh4 cpu;
static unsigned long hw_count;
static unsigned long MAX_HW = 200000;

extern void (*hle_trace)(const char *kind, u32 addr, u32 value, int size);
extern u64 hle_frames;

/* per-register access counts, to see what a loop is polling */
#define NCOUNT 8192
static struct { u32 key; const char *kind; u32 addr, last; unsigned long n; } counts[NCOUNT];

static void count_access(const char *kind, u32 addr, u32 value)
{
	u32 key = (addr & 0x1FFFFFFC) ^ (kind[0] == 'W' ? 0x80000000u : 0) ^ (kind[0] == 'S' ? 0x40000000u : 0);
	u32 h = (key * 2654435761u) % NCOUNT;
	for (int probe = 0; probe < NCOUNT; probe++, h = (h + 1) % NCOUNT) {
		if (counts[h].n == 0) { counts[h].key = key; counts[h].kind = kind; counts[h].addr = addr; }
		if (counts[h].key == key) { counts[h].n++; counts[h].last = value; return; }
	}
}

static void print_counts(void)
{
	extern u64 hle_renders, ta_yuv_bytes;
	extern u32 ta_lists_ended;
	fprintf(stderr, "--- frames %llu, renders %llu, TA lists ended %u, YUV bytes %llu, cycles %llu ---\n",
		(unsigned long long)hle_frames, (unsigned long long)hle_renders, ta_lists_ended,
		(unsigned long long)ta_yuv_bytes, (unsigned long long)cpu.cycles);
	extern u32 coro_switches, coro_fibers, sound_cmd_count;
	fprintf(stderr, "--- task fibers %u, task switches %u, sound commands %u ---\n",
		coro_fibers, coro_switches, sound_cmd_count);
	fprintf(stderr, "--- most-accessed registers ---\n");
	for (int top = 0; top < 15; top++) {
		int best = -1;
		for (int i = 0; i < NCOUNT; i++)
			if (counts[i].n && (best < 0 || counts[i].n > counts[best].n)) best = i;
		if (best < 0) break;
		fprintf(stderr, "  %8lu  %s %08X  last=%08X\n", counts[best].n, counts[best].kind, counts[best].addr, counts[best].last);
		counts[best].n = 0;
	}
}

extern u32 hle_read(u32 addr, int size);

/* HOTD2_SHOTS=100,500 renders those STARTRENDER calls to shot_<n>.ppm */
static int listed(const char *spec, unsigned long long n)
{
	for (const char *p = spec; p && *p; ) {
		if (strtoull(p, NULL, 10) == n) return 1;
		const char *comma = strchr(p, ',');
		if (!comma) break;
		p = comma + 1;
	}
	return 0;
}

static void shot_on_render(u64 render, const u8 *list, u32 len)
{
	extern void pvr_render_soft(const u8 *list, u32 len, u8 *rgba);
	static u8 rgba[640 * 480 * 4];
	if (!listed(getenv("HOTD2_SHOTS"), render)) return;
	pvr_render_soft(list, len, rgba);
	char name[64];
	snprintf(name, sizeof name, "shot_%llu.ppm", (unsigned long long)render);
	FILE *f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n640 480\n255\n");
	for (int i = 0; i < 640 * 480; i++) fwrite(&rgba[4 * i], 1, 3, f);
	fclose(f);
	fprintf(stderr, "shot: render %llu, %u list bytes -> %s\n", (unsigned long long)render, len, name);
}

/* HOTD2_DUMP_FRAMES=700,900 writes ram_<frame>.bin, same as the patched Flycast */
static u64 frame_limit;   /* HOTD2_FRAMES: stop after this many frames */

static void stop(const char *why);

static void dump_on_frame(u64 frame)
{
	if (frame_limit && frame >= frame_limit)
		stop("frame limit reached");
	static const char *spec;
	if (!spec && !(spec = getenv("HOTD2_DUMP_FRAMES"))) spec = "";
	for (const char *p = spec; *p; ) {
		if (strtoull(p, NULL, 10) == frame) {
			char name[64];
			snprintf(name, sizeof name, "hram_%llu.bin", (unsigned long long)frame);
			FILE *f = fopen(name, "wb");
			if (f) { fwrite(ram, 1, RAM_SIZE, f); fclose(f); }
		}
		const char *comma = strchr(p, ',');
		if (!comma) break;
		p = comma + 1;
	}
}

static void print_irq_state(void)
{
	static const char *names[3] = { "IML2", "IML4", "IML6" };
	void (*saved)(const char *, u32, u32, int) = hle_trace;
	hle_trace = NULL;
	fprintf(stderr, "--- interrupt state ---\n  ISTNRM=%08X ISTEXT=%08X ISTERR=%08X\n",
		hle_read(0xA05F6900u, 4), hle_read(0xA05F6904u, 4), hle_read(0xA05F6908u, 4));
	for (int k = 0; k < 3; k++) {
		u32 base = 0xA05F6910u + 0x10 * k;
		fprintf(stderr, "  %s NRM=%08X EXT=%08X ERR=%08X\n", names[k],
			hle_read(base, 4), hle_read(base + 4, 4), hle_read(base + 8, 4));
	}
	fprintf(stderr, "  SR=%08X VBR=%08X; BIOS callbacks:", sr_get(&cpu), cpu.vbr);
	for (u32 slot = 0; slot < 0x40; slot++) {
		u32 h = rd32(cpu.vbr + 0x1C0 + slot * 4);
		if (h) fprintf(stderr, " [INTEVT %03X]=%08X", (slot * 4 + 0x1C0 - 0x1C0) << 3, h);
	}
	fprintf(stderr, "\n");
	hle_trace = saved;
}

static void stop(const char *why)
{
	fprintf(stderr, "stop: %s (hw accesses: %lu)\n", why, hw_count);
	FILE *dump = fopen("ram_stop.bin", "wb");   /* for comparing with Flycast's RAM dumps */
	if (dump) { fwrite(ram, 1, RAM_SIZE, dump); fclose(dump); }
	print_counts();
	print_irq_state();
	extern void ta_debug(void);
	ta_debug();
	exit(0);
}

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
	count_access(kind, addr, value);
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
	if (getenv("HW_BUDGET"))
		MAX_HW = strtoul(getenv("HW_BUDGET"), NULL, 0);
	extern void hle_init(Sh4 *c);
	extern int cart_load(const char *path);
	const char *cart = getenv("HOTD2_CART") ? getenv("HOTD2_CART") : "cart.bin";
	if (!cart_load(cart))
		fprintf(stderr, "warning: no cart image (%s); cart reads return 0xFF\n", cart);
	hle_init(&cpu);
	extern void (*hle_on_frame)(u64 frame);
	hle_on_frame = dump_on_frame;
	extern void (*hle_on_render)(u64 render, const u8 *list, u32 len);
	hle_on_render = shot_on_render;
	/* HOTD2_TRACE=0 turns off per-access tracing (and the access budget) to measure speed */
	hle_trace = (getenv("HOTD2_TRACE") && getenv("HOTD2_TRACE")[0] == '0') ? NULL : hw_trace;
	if (getenv("HOTD2_FRAMES"))
		frame_limit = strtoull(getenv("HOTD2_FRAMES"), NULL, 10);
	signal(SIGTERM, dump_ring);
	signal(SIGINT, dump_ring);
	fprintf(stderr, "running from %08X\n", entry);
	sh4_dispatch(&cpu, entry);
	stop("entry function returned");
}
