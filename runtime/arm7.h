/* ARM7 (ARMv4, ARM state) interpreter for the AICA sound CPU.
 * The ARM sees sound RAM at 0x00000000 and the AICA registers at 0x00800000. */
#pragma once
#include "sh4ctx.h"

typedef struct Arm7 {
	u32 r[16];
	u32 cpsr, spsr;
	/* banked registers: index by mode slot (0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und) */
	u32 bank_r13[6], bank_r14[6], bank_spsr[6];
	u32 fiq_r8_12[5], usr_r8_12[5];
	int fiq_line;          /* FIQ input level from the AICA interrupt controller */
	u64 cycles;
} Arm7;

extern Arm7 arm7;

void arm7_reset(void);
/* Run about `cycles` instructions (stops early never); handles FIQ entry. */
void arm7_run(int cycles);

/* memory, provided by aica.c */
u32 arm_read32(u32 a);
u8 arm_read8(u32 a);
void arm_write32(u32 a, u32 v);
void arm_write8(u32 a, u8 v);
