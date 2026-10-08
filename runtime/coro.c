/* Guest coroutines on host fibers, see coro.h. Native build: ucontext.
 * Browser build: only same-stack longjmp is supported so far (HOTD2 has not
 * needed a real stack switch yet); a switch request reports and aborts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coro.h"
#ifdef HOTD2_DIFF
extern int diff_trial;
extern jmp_buf diff_abort;
#define DIFF_GUARD() do { if (diff_trial) longjmp(diff_abort, 1); } while (0)
#else
#define DIFF_GUARD() do {} while (0)
#endif

#ifdef __EMSCRIPTEN__
#define MAX_CTX 256
typedef struct { u32 buf; int bound; jmp_buf jb; } Ctx;
static Ctx ctxs[MAX_CTX];
u32 coro_switches, coro_fibers;

static Ctx *lookup(u32 buf, int create)
{
	buf &= 0x1FFFFFFFu;
	Ctx *free_slot = NULL;
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i].buf == buf) return &ctxs[i];
		if (!ctxs[i].buf && !free_slot) free_slot = &ctxs[i];
	}
	if (!create || !free_slot) return NULL;
	free_slot->buf = buf;
	return free_slot;
}

jmp_buf *coro_bind(u32 buf)
{
	DIFF_GUARD();
	Ctx *x = lookup(buf, 1);
	if (!x) { fprintf(stderr, "coro: too many contexts\n"); abort(); }
	x->bound = 1;
	return &x->jb;
}

void coro_restore(Sh4 *c, u32 buf)
{
	DIFF_GUARD();
	(void)c;
	Ctx *x = lookup(buf, 0);
	if (x && x->bound)
		longjmp(x->jb, 1);
	fprintf(stderr, "coro: restore of unsaved context %08X needs a fiber switch (not supported in the browser yet)\n", buf);
	abort();
}
#else
#include <ucontext.h>

#define FIBER_STACK (2u << 20)
#define MAX_CTX 256

typedef struct Fiber Fiber;
typedef struct {
	u32 buf;          /* guest context buffer address (0 = free slot) */
	Fiber *owner;     /* fiber whose frame holds the setjmp */
	int bound;        /* jb is valid */
	jmp_buf jb;
} Ctx;

struct Fiber {
	ucontext_t uc;
	Ctx *pending;     /* where to longjmp when this fiber is switched back to */
	u8 *stack;
};

static Fiber main_fiber;
static Fiber *current = &main_fiber;
static Ctx ctxs[MAX_CTX];
static Sh4 *fiber_cpu;
u32 coro_switches, coro_fibers;

void sh4_dispatch(Sh4 *c, u32 target);

static Ctx *lookup(u32 buf, int create)
{
	buf &= 0x1FFFFFFFu;
	Ctx *free_slot = NULL;
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i].buf == buf) return &ctxs[i];
		if (!ctxs[i].buf && !free_slot) free_slot = &ctxs[i];
	}
	if (!create) return NULL;
	if (!free_slot) { fprintf(stderr, "coro: too many contexts\n"); abort(); }
	free_slot->buf = buf;
	return free_slot;
}

jmp_buf *coro_bind(u32 buf)
{
	DIFF_GUARD();
	Ctx *x = lookup(buf, 1);
	x->owner = current;
	x->bound = 1;
	return &x->jb;
}

/* Entry of a fresh task fiber: the restore already loaded the guest registers,
 * so run from the restored PR (the task's start address). */
static void fiber_entry(void)
{
	u32 start = fiber_cpu->pr;
	sh4_dispatch(fiber_cpu, start);
	fprintf(stderr, "coro: task starting at %08X returned; no context to return to\n", start);
	abort();
}

static void switch_to(Fiber *to)
{
	Fiber *from = current;
	current = to;
	coro_switches++;
	swapcontext(&from->uc, &to->uc);
	/* back on `from`: someone restored a context this fiber owns */
	Ctx *p = current->pending;
	current->pending = NULL;
	longjmp(p->jb, 1);
}

void coro_restore(Sh4 *c, u32 buf)
{
	DIFF_GUARD();
	Ctx *x = lookup(buf, 0);
	if (x && x->bound && x->owner == current)
		longjmp(x->jb, 1);                 /* same fiber: plain longjmp down the stack */
	if (x && x->bound) {
		x->owner->pending = x;
		switch_to(x->owner);
	}
	/* never saved: a new task. Its buffer holds the entry PC and stack. */
	Fiber *f = calloc(1, sizeof(Fiber));
	f->stack = malloc(FIBER_STACK);
	getcontext(&f->uc);
	f->uc.uc_stack.ss_sp = f->stack;
	f->uc.uc_stack.ss_size = FIBER_STACK;
	f->uc.uc_link = NULL;
	makecontext(&f->uc, fiber_entry, 0);
	fiber_cpu = c;
	coro_fibers++;
	switch_to(f);
	abort(); /* unreachable: switch_to always longjmps when resumed */
}
#endif /* !__EMSCRIPTEN__ */
