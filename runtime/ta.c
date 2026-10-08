/* PVR2 Tile Accelerator input.
 *
 * The TA area (0x10000000-0x11FFFFFF) is fed 32 bytes at a time by store
 * queues and channel-2 DMA:
 *   0x10000000-0x107FFFFF  polygon/display-list FIFO  -> recorded for the renderer
 *   0x10800000-0x10FFFFFF  YUV converter              -> not yet emulated
 *   0x11000000-0x11FFFFFF  direct texture write       -> VRAM (64-bit path)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh4ctx.h"
#include "ta.h"

extern u8 vram[0x1000000];

u8 *ta_list;          /* display-list bytes received since the last ta_list_reset() */
u32 ta_list_len;
static u32 ta_list_cap;
u64 ta_yuv_bytes;

/* Parameter-stream state, just enough to find list boundaries:
 * the current list type and whether the next 32-byte block continues a 64-byte parameter. */
static int list_type = -1;   /* -1: between lists */
static int vertex_64;        /* vertices of the current object are 64 bytes */
static int skip_next;
u32 ta_lists_ended;          /* count, for debugging */

void hle_raise_normal(int bit);

static const int list_end_irq[5] = { 7, 8, 9, 10, 21 }; /* opaque, op. mod, trans, tr. mod, punch-through */

void ta_list_reset(void)
{
	ta_list_len = 0;
	list_type = -1;
	skip_next = 0;
}

static u32 recent_pcw[16];
static unsigned recent_pos;

void ta_debug(void)
{
	fprintf(stderr, "--- TA: open list %d, %u bytes this frame, skip_next %d, vertex_64 %d; last PCWs:",
		list_type, ta_list_len, skip_next, vertex_64);
	for (unsigned k = 0; k < 16; k++)
		fprintf(stderr, " %08X", recent_pcw[(recent_pos + k) % 16]);
	fprintf(stderr, "\n");
}

static void ta_param(const u8 *p)
{
	u32 pcw;
	memcpy(&pcw, p, 4);
	recent_pcw[recent_pos++ % 16] = pcw;
	u32 para = pcw >> 29;
	int tex = (pcw >> 3) & 1, offset = (pcw >> 2) & 1, uv16 = pcw & 1;
	int col = (pcw >> 4) & 3, volume = (pcw >> 6) & 1;

	switch (para) {
	case 0: /* end of list */
		if (list_type >= 0 && list_type < 5) {
			hle_raise_normal(list_end_irq[list_type]);
			ta_lists_ended++;
		}
		list_type = -1;
		break;
	case 4: /* polygon or modifier-volume header */
	case 5: /* sprite header */
		if (list_type < 0)
			list_type = (pcw >> 24) & 7;
		if (list_type == 1 || list_type == 3) {    /* modifier volume */
			vertex_64 = 1;
		} else if (para == 5) {
			vertex_64 = 1;
		} else {
			/* 64-byte headers: polygon type 2 (intensity + offset, textured) and type 4 (two volumes, intensity) */
			if ((!volume && col == 2 && tex && offset) || (volume && col == 2))
				skip_next = 1;
			/* 64-byte vertices: textured floating colour (5, 6) and textured two-volume (11-14) */
			vertex_64 = tex && ((!volume && col == 1) || volume);
		}
		(void)uv16;
		break;
	case 7: /* vertex */
		if (vertex_64)
			skip_next = 1;
		break;
	default: /* user clip, object list set: 32 bytes */
		break;
	}
}

static void ta_fifo(const u8 *data, u32 len)
{
	if (ta_list_len + len > ta_list_cap) {
		ta_list_cap = (ta_list_len + len) * 2 + 0x10000;
		ta_list = realloc(ta_list, ta_list_cap);
	}
	memcpy(ta_list + ta_list_len, data, len);
	ta_list_len += len;
	for (u32 k = 0; k + 32 <= len; k += 32) {
		if (skip_next) { skip_next = 0; continue; }
		ta_param(data + k);
	}
}

void ta_write(u32 addr, const u8 *data, u32 len)
{
	u32 p = addr & 0x1FFFFFFFu;
	if (p < 0x10000000u || p >= 0x12000000u) {
		/* not TA space: ordinary memory */
		for (u32 k = 0; k < len; k++) wr8(addr + k, data[k]);
	} else if (p < 0x10800000u) {
		ta_fifo(data, len);
	} else if (p < 0x11000000u) {
		ta_yuv_bytes += len;
	} else {
		u32 o = p & 0xFFFFFFu;
		for (u32 k = 0; k < len; k++) vram[(o + k) & 0xFFFFFFu] = data[k];
	}
}
