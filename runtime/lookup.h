/* Address -> recompiled function, for indirect calls (sh4_dispatch).
 *
 * Indirect calls are hot: HOTD2's frame delay loop alone calls a function
 * pointer ~190,000 times per frame. A direct table over the program image
 * (one slot per 2-byte instruction) makes the lookup O(1); anything outside
 * the image falls back to a binary search of func_table.
 */
#pragma once
#include <stdlib.h>
#include "sh4ctx.h"
#include "funcs.h"

#define LOOKUP_BASE 0x0C020000u
#define LOOKUP_SIZE 0x00126000u   /* covers the program image (0x125E10 bytes) */

static void (*func_search(u32 t))(Sh4 *)
{
	u32 lo = 0, hi = func_count;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		u32 a = func_table[mid].addr & 0x1FFFFFFF;
		if (a == t) return func_table[mid].fn;
		if (a < t) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

static void (*func_lookup(u32 target))(Sh4 *)
{
	static void (**direct)(Sh4 *);
	u32 t = target & 0x1FFFFFFF;
	if (!direct) {
		direct = calloc(LOOKUP_SIZE / 2, sizeof *direct);
		for (u32 i = 0; i < func_count; i++) {
			u32 a = (func_table[i].addr & 0x1FFFFFFF) - LOOKUP_BASE;
			if (a < LOOKUP_SIZE) direct[a / 2] = func_table[i].fn;
		}
	}
	u32 o = t - LOOKUP_BASE;
	if (o < LOOKUP_SIZE && !(o & 1)) return direct[o / 2];
	return func_search(t);
}
