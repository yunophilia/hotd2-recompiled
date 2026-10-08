/* AICA sound chip: registers, timers, ARM interrupt controller, 64 voices.
 * Sample clock 44100 Hz; aica_advance() runs the ARM7 and synthesises audio. */
#pragma once
#include "sh4ctx.h"

#define AICA_RATE 44100
#define AICA_RING 16384          /* stereo frames in the output ring */

extern int aica_enabled;         /* 0: keep the old stubs (no ARM emulation) */
extern s16 aica_ring[AICA_RING * 2];
extern volatile u32 aica_ring_write;   /* frames produced (monotonic) */

u32 aica_reg_read(u32 offset, int size);     /* offset within the 0x10000 register block */
void aica_reg_write(u32 offset, u32 value, int size);

/* Advance the chip by SH-4 cycles elapsed (called from the scheduler). */
void aica_advance(u64 sh4_cycles);
