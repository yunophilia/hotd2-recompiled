#pragma once
#include "sh4ctx.h"

extern u8 *ta_list;
extern u32 ta_list_len;
extern u64 ta_yuv_bytes;

/* Write len bytes (a multiple of 32) to TA space at physical/guest address addr. */
void ta_write(u32 addr, const u8 *data, u32 len);
void ta_list_reset(void);
