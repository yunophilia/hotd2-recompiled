/* Guest coroutines (HOTD2's task system) on host fibers.
 *
 * The game switches tasks with a setjmp/longjmp-like pair:
 *   0x0C0BA0C0 save-context(r4 = buffer): stores all registers, returns
 *   0x0C0BA126 restore-context(r4 = buffer, r5 = value): reloads them and
 *              "returns again" from the matching save, on that task's stack.
 * Each guest task runs on its own host fiber. The recompiler wraps every call
 * to save-context in CORO_SAVE (a host setjmp in the calling frame) and
 * routes restore-context to coro_restore().
 */
#pragma once
#include <setjmp.h>
#include "sh4ctx.h"

#define CORO_SAVE_ADDR    0x0C0BA0C0u
#define CORO_RESTORE_ADDR 0x0C0BA126u

/* Bind guest context buffer `buf` to the current fiber; returns the host
 * jump buffer to setjmp on, in the frame that called save-context. */
jmp_buf *coro_bind(u32 buf);
/* Switch to the context saved in `buf` (guest registers already reloaded). */
void coro_restore(Sh4 *c, u32 buf) __attribute__((noreturn));

void f_0c0ba0c0(Sh4 *c);
#define CORO_SAVE(c) do { \
	jmp_buf *jb_ = coro_bind((c)->r[4]); \
	if (!setjmp(*jb_)) f_0c0ba0c0(c); \
} while (0)
