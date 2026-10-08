#pragma once
#include "sh4ctx.h"

int cart_load(const char *path);
/* NAOMI cart registers 0x005F7000-0x005F70FF (physical); return 1 if handled */
int cart_read_reg(u32 phys, u32 *value);
int cart_write_reg(u32 phys, u32 value);
/* G1 DMA: copy len bytes from the current DMA offset to guest address dst */
void cart_dma(u32 dst, u32 len);
