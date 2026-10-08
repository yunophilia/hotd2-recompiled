/* Browser entry points for the recompiled game (Emscripten build).
 *
 * JavaScript loads the user's ROM set, hands the program image and the cart
 * image over with web_set_program / web_set_cart, then calls web_start, which
 * runs the game on its own pthread paced to 60 Hz. Frames are published by
 * glframe_build at every STARTRENDER and drawn by web/gl.js on the main thread.
 */
#include <emscripten.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sh4ctx.h"
#include "funcs.h"
#include "jvs.h"
#include "glframe.h"

u8 *ram;
static Sh4 cpu;
static u8 *program;
static u32 program_len;
static volatile u32 game_frames, game_renders;

void hle_init(Sh4 *c);
void cart_set_image(u8 *image, u32 size);
extern void (*hle_on_frame)(u64 frame);
extern void (*hle_on_render)(u64 render, const u8 *list, u32 len);

void sh4_dispatch(Sh4 *c, u32 target)
{
	u32 lo = 0, hi = func_count, t = target & 0x1FFFFFFF;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		u32 a = func_table[mid].addr & 0x1FFFFFFF;
		if (a == t) { func_table[mid].fn(c); return; }
		if (a < t) lo = mid + 1; else hi = mid;
	}
	fprintf(stderr, "hotd2: no recompiled function at %08X (pr=%08X)\n", target, c->pr);
	abort();
}

void sh4_unimplemented(Sh4 *c, u32 pc, u16 op)
{
	fprintf(stderr, "hotd2: unimplemented opcode %04X at %08X (pr=%08X)\n", op, pc, c->pr);
	abort();
}

/* Keep the guest at 60 frames per second of wall-clock time. */
static void pace(u64 frame)
{
	static double start;
	static u64 base;
	game_frames = (u32)frame;
	double now = emscripten_get_now();
	if (!start || now - (start + (frame - base) * (1000.0 / 60)) > 250) {
		start = now;           /* first frame, or fell far behind: resynchronise */
		base = frame;
		return;
	}
	double target = start + (frame - base) * (1000.0 / 60);
	if (target > now)
		usleep((useconds_t)((target - now) * 1000));
}

static void render(u64 n, const u8 *list, u32 len)
{
	game_renders = (u32)n;
	glframe_build(list, len);
}

static void *game_thread(void *arg)
{
	(void)arg;
	ram = calloc(1, RAM_SIZE);
	memcpy(ram + 0x20000, program, program_len < 0x125E10 ? program_len : 0x125E10);
	cpu.sr_rest = 0x40000000u;
	cpu.fpscr = 0x00040001u;
	cpu.r[15] = 0x0CA553C0u;
	hle_init(&cpu);
	hle_on_frame = pace;
	hle_on_render = render;
	sh4_dispatch(&cpu, 0x0C020000u);
	fprintf(stderr, "hotd2: entry returned\n");
	return NULL;
}

EMSCRIPTEN_KEEPALIVE void *web_alloc(u32 n) { return malloc(n); }
EMSCRIPTEN_KEEPALIVE void web_set_program(u8 *p, u32 n) { program = p; program_len = n; }
EMSCRIPTEN_KEEPALIVE void web_set_cart(u8 *p, u32 n) { cart_set_image(p, n); }
EMSCRIPTEN_KEEPALIVE JvsInput *web_input(void) { return &jvs_input; }
EMSCRIPTEN_KEEPALIVE u32 web_frames(void) { return game_frames; }
EMSCRIPTEN_KEEPALIVE u32 web_renders(void) { return game_renders; }

EMSCRIPTEN_KEEPALIVE void web_start(void)
{
	pthread_t t;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 8 << 20);   /* deep recompiled call chains */
	pthread_create(&t, &a, game_thread, NULL);
}

u32 pvr_reg(u32 phys);
EMSCRIPTEN_KEEPALIVE u32 web_reg(u32 phys) { return pvr_reg(phys); }

/* frame and texture access for web/gl.js */
EMSCRIPTEN_KEEPALIVE GlFrame *web_frame(void) { return glframe_latest(); }
EMSCRIPTEN_KEEPALIVE u32 web_tex_count(void) { return glf_ntex; }
EMSCRIPTEN_KEEPALIVE GlTex *web_tex(u32 i) { return &glf_tex[i]; }
