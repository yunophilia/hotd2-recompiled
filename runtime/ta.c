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

void ta_list_reset(void) { ta_list_len = 0; }

static void ta_fifo(const u8 *data, u32 len)
{
	if (ta_list_len + len > ta_list_cap) {
		ta_list_cap = (ta_list_len + len) * 2 + 0x10000;
		ta_list = realloc(ta_list, ta_list_cap);
	}
	memcpy(ta_list + ta_list_len, data, len);
	ta_list_len += len;
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
