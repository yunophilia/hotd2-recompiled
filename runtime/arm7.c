/* ARM7 interpreter for the AICA sound CPU (ARM state only; the AICA's ARM7DI
 * has no Thumb). See arm7.h. */
#include <string.h>
#include "arm7.h"

Arm7 arm7;
u64 arm7_fiqs;

#define N_FLAG (1u << 31)
#define Z_FLAG (1u << 30)
#define C_FLAG (1u << 29)
#define V_FLAG (1u << 28)
#define I_FLAG (1u << 7)
#define F_FLAG (1u << 6)

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };

static int slot(u32 mode)
{
	switch (mode & 0x1F) {
	case M_FIQ: return 1;
	case M_IRQ: return 2;
	case M_SVC: return 3;
	case M_ABT: return 4;
	case M_UND: return 5;
	default: return 0;
	}
}

/* Switch the visible registers to a new mode's bank. */
static void set_mode(u32 new_cpsr)
{
	Arm7 *a = &arm7;
	int from = slot(a->cpsr), to = slot(new_cpsr);
	if (from != to) {
		a->bank_r13[from] = a->r[13];
		a->bank_r14[from] = a->r[14];
		a->bank_spsr[from] = a->spsr;
		if (from == 1 && to != 1) {
			memcpy(a->fiq_r8_12, &a->r[8], sizeof a->fiq_r8_12);
			memcpy(&a->r[8], a->usr_r8_12, sizeof a->usr_r8_12);
		} else if (from != 1 && to == 1) {
			memcpy(a->usr_r8_12, &a->r[8], sizeof a->usr_r8_12);
			memcpy(&a->r[8], a->fiq_r8_12, sizeof a->fiq_r8_12);
		}
		a->r[13] = a->bank_r13[to];
		a->r[14] = a->bank_r14[to];
		a->spsr = a->bank_spsr[to];
	}
	a->cpsr = new_cpsr;
}

void arm7_reset(void)
{
	memset(&arm7, 0, sizeof arm7);
	arm7.cpsr = M_SVC | I_FLAG | F_FLAG;
	arm7.r[15] = 0;
}

static int cond_pass(u32 cond)
{
	u32 f = arm7.cpsr;
	int n = !!(f & N_FLAG), z = !!(f & Z_FLAG), c = !!(f & C_FLAG), v = !!(f & V_FLAG);
	switch (cond) {
	case 0x0: return z;
	case 0x1: return !z;
	case 0x2: return c;
	case 0x3: return !c;
	case 0x4: return n;
	case 0x5: return !n;
	case 0x6: return v;
	case 0x7: return !v;
	case 0x8: return c && !z;
	case 0x9: return !c || z;
	case 0xA: return n == v;
	case 0xB: return n != v;
	case 0xC: return !z && n == v;
	case 0xD: return z || n != v;
	case 0xE: return 1;
	default: return 0;
	}
}

static inline void set_nz(u32 v)
{
	arm7.cpsr = (arm7.cpsr & ~(N_FLAG | Z_FLAG)) | (v & N_FLAG) | (v ? 0 : Z_FLAG);
}
static inline void set_c(int c) { arm7.cpsr = c ? arm7.cpsr | C_FLAG : arm7.cpsr & ~C_FLAG; }
static inline void set_v(int v) { arm7.cpsr = v ? arm7.cpsr | V_FLAG : arm7.cpsr & ~V_FLAG; }

/* register value as an operand. While an instruction executes r15 already holds
 * its address + 4, so the architectural PC (address + 8) is r15 + 4 (+4 more for
 * register-specified shifts and stored PCs). */
static inline u32 reg(int n, int extra) { return n == 15 ? arm7.r[15] + 4 + extra : arm7.r[n]; }

/* barrel shifter; *carry gets the shifter carry-out */
static u32 shift_op(u32 instr, int *carry)
{
	int c = !!(arm7.cpsr & C_FLAG);
	if (instr & (1u << 25)) {                       /* rotated immediate */
		u32 imm = instr & 0xFF, rot = ((instr >> 8) & 15) * 2;
		u32 v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
		*carry = rot ? (int)(v >> 31) : c;
		return v;
	}
	int by_reg = (instr >> 4) & 1;
	u32 rm = reg((int)(instr & 15), by_reg ? 4 : 0);
	int type = (instr >> 5) & 3;
	u32 amt;
	if (by_reg) {
		amt = reg((int)((instr >> 8) & 15), 0) & 0xFF;
		if (amt == 0) { *carry = c; return rm; }
	} else {
		amt = (instr >> 7) & 31;
		if (amt == 0) {
			switch (type) {
			case 0: *carry = c; return rm;                       /* LSL #0 */
			case 1: *carry = (int)(rm >> 31); return 0;          /* LSR #32 */
			case 2: *carry = (int)(rm >> 31); return (u32)((s32)rm >> 31);  /* ASR #32 */
			default: *carry = (int)(rm & 1); return (rm >> 1) | ((u32)c << 31);  /* RRX */
			}
		}
	}
	switch (type) {
	case 0:
		if (amt < 32) { *carry = (int)((rm >> (32 - amt)) & 1); return rm << amt; }
		*carry = amt == 32 ? (int)(rm & 1) : 0; return 0;
	case 1:
		if (amt < 32) { *carry = (int)((rm >> (amt - 1)) & 1); return rm >> amt; }
		*carry = amt == 32 ? (int)(rm >> 31) : 0; return 0;
	case 2:
		if (amt < 32) { *carry = (int)(((s32)rm >> (amt - 1)) & 1); return (u32)((s32)rm >> amt); }
		*carry = (int)(rm >> 31); return (u32)((s32)rm >> 31);
	default: {
		u32 r = amt & 31;
		u32 v = r ? (rm >> r) | (rm << (32 - r)) : rm;
		*carry = (int)(v >> 31);
		return v;
	}
	}
}

static void write_pc(u32 v) { arm7.r[15] = v & ~3u; }

static void data_processing(u32 instr)
{
	int op = (instr >> 21) & 15, s = (instr >> 20) & 1;
	int rn = (instr >> 16) & 15, rd = (instr >> 12) & 15;
	int sc;
	u32 op2 = shift_op(instr, &sc);
	int by_reg = !(instr & (1u << 25)) && ((instr >> 4) & 1);
	u32 a = reg(rn, by_reg ? 4 : 0), r = 0;
	int c = !!(arm7.cpsr & C_FLAG), logical = 0, write = 1;
	u64 wide;
	switch (op) {
	case 0x0: r = a & op2; logical = 1; break;
	case 0x1: r = a ^ op2; logical = 1; break;
	case 0x2: r = a - op2; if (s) { set_c(a >= op2); set_v(((a ^ op2) & (a ^ r)) >> 31); } break;
	case 0x3: r = op2 - a; if (s) { set_c(op2 >= a); set_v(((op2 ^ a) & (op2 ^ r)) >> 31); } break;
	case 0x4: wide = (u64)a + op2; r = (u32)wide; if (s) { set_c((int)(wide >> 32)); set_v((~(a ^ op2) & (a ^ r)) >> 31); } break;
	case 0x5: wide = (u64)a + op2 + (u32)c; r = (u32)wide; if (s) { set_c((int)(wide >> 32)); set_v((~(a ^ op2) & (a ^ r)) >> 31); } break;
	case 0x6: wide = (u64)a - op2 - (u32)!c; r = (u32)wide; if (s) { set_c((u64)a >= (u64)op2 + (u32)!c); set_v(((a ^ op2) & (a ^ r)) >> 31); } break;
	case 0x7: wide = (u64)op2 - a - (u32)!c; r = (u32)wide; if (s) { set_c((u64)op2 >= (u64)a + (u32)!c); set_v(((op2 ^ a) & (op2 ^ r)) >> 31); } break;
	case 0x8: r = a & op2; logical = 1; write = 0; break;
	case 0x9: r = a ^ op2; logical = 1; write = 0; break;
	case 0xA: r = a - op2; write = 0; set_c(a >= op2); set_v(((a ^ op2) & (a ^ r)) >> 31); break;
	case 0xB: wide = (u64)a + op2; r = (u32)wide; write = 0; set_c((int)(wide >> 32)); set_v((~(a ^ op2) & (a ^ r)) >> 31); break;
	case 0xC: r = a | op2; logical = 1; break;
	case 0xD: r = op2; logical = 1; break;
	case 0xE: r = a & ~op2; logical = 1; break;
	case 0xF: r = ~op2; logical = 1; break;
	}
	if (s || !write) {
		if (rd == 15 && s && write) {          /* restore CPSR from SPSR */
			set_mode(arm7.spsr);
		} else {
			set_nz(r);
			if (logical) set_c(sc);
		}
	}
	if (write) {
		if (rd == 15) write_pc(r);
		else arm7.r[rd] = r;
	}
}

static void multiply(u32 instr)
{
	int rd = (instr >> 16) & 15, rn = (instr >> 12) & 15, rs = (instr >> 8) & 15, rm = instr & 15;
	int s = (instr >> 20) & 1;
	if (((instr >> 23) & 1) == 0) {           /* MUL / MLA */
		u32 r = arm7.r[rm] * arm7.r[rs];
		if ((instr >> 21) & 1) r += arm7.r[rn];
		arm7.r[rd] = r;
		if (s) set_nz(r);
		return;
	}
	/* long multiplies: RdHi = rd, RdLo = rn */
	u64 r;
	if ((instr >> 22) & 1) r = (u64)((s64)(s32)arm7.r[rm] * (s64)(s32)arm7.r[rs]);
	else r = (u64)arm7.r[rm] * arm7.r[rs];
	if ((instr >> 21) & 1) r += ((u64)arm7.r[rd] << 32) | arm7.r[rn];
	arm7.r[rn] = (u32)r;
	arm7.r[rd] = (u32)(r >> 32);
	if (s) {
		arm7.cpsr = (arm7.cpsr & ~(N_FLAG | Z_FLAG)) | ((u32)(r >> 32) & N_FLAG) | (r ? 0 : Z_FLAG);
	}
}

static u32 load32(u32 addr)
{
	u32 v = arm_read32(addr & ~3u);
	u32 rot = (addr & 3) * 8;          /* unaligned loads rotate */
	return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

static void single_transfer(u32 instr)
{
	int p = (instr >> 24) & 1, u = (instr >> 23) & 1, b = (instr >> 22) & 1, w = (instr >> 21) & 1, l = (instr >> 20) & 1;
	int rn = (instr >> 16) & 15, rd = (instr >> 12) & 15;
	u32 off;
	/* For loads/stores bit 25 set means a (shifted) REGISTER offset - the opposite of
	 * data processing - so clear it before using the shared barrel shifter. */
	if (instr & (1u << 25)) { int c; off = shift_op(instr & ~((1u << 25) | (1u << 4)), &c); }
	else off = instr & 0xFFF;
	u32 base = reg(rn, 0);
	u32 addr = p ? (u ? base + off : base - off) : base;
	if (l) {
		u32 v = b ? arm_read8(addr) : load32(addr);
		if (!p || w) arm7.r[rn] = u ? base + off : base - off;
		if (rd == 15) write_pc(v); else arm7.r[rd] = v;
	} else {
		u32 v = reg(rd, 4);
		if (b) arm_write8(addr, (u8)v); else arm_write32(addr & ~3u, v);
		if (!p || w) arm7.r[rn] = u ? base + off : base - off;
	}
}

static void halfword_transfer(u32 instr)
{
	int p = (instr >> 24) & 1, u = (instr >> 23) & 1, imm = (instr >> 22) & 1, w = (instr >> 21) & 1, l = (instr >> 20) & 1;
	int rn = (instr >> 16) & 15, rd = (instr >> 12) & 15, sh = (instr >> 5) & 3;
	u32 off = imm ? (((instr >> 4) & 0xF0) | (instr & 15)) : arm7.r[instr & 15];
	u32 base = reg(rn, 0);
	u32 addr = p ? (u ? base + off : base - off) : base;
	if (l) {
		u32 v;
		if (sh == 1) v = (u32)arm_read8(addr) | (u32)arm_read8(addr + 1) << 8;
		else if (sh == 2) v = (u32)(s32)(s8)arm_read8(addr);
		else v = (u32)(s32)(s16)((u32)arm_read8(addr) | (u32)arm_read8(addr + 1) << 8);
		if (!p || w) arm7.r[rn] = u ? base + off : base - off;
		arm7.r[rd] = v;
	} else {
		u32 v = reg(rd, 4);
		arm_write8(addr, (u8)v);
		arm_write8(addr + 1, (u8)(v >> 8));
		if (!p || w) arm7.r[rn] = u ? base + off : base - off;
	}
}

static void block_transfer(u32 instr)
{
	int p = (instr >> 24) & 1, u = (instr >> 23) & 1, s = (instr >> 22) & 1, w = (instr >> 21) & 1, l = (instr >> 20) & 1;
	int rn = (instr >> 16) & 15;
	u32 list = instr & 0xFFFF;
	int count = __builtin_popcount(list);
	u32 base = arm7.r[rn];
	u32 start = u ? base : base - 4u * (u32)count;
	if (p == u) start += 4;      /* pre-increment / post-decrement adjust */
	u32 final = u ? base + 4u * (u32)count : base - 4u * (u32)count;
	/* S bit without PC in an LDM (or any STM): user-bank transfer */
	int user_bank = s && !(l && (list & 0x8000));
	u32 saved_mode = arm7.cpsr;
	if (user_bank) set_mode((arm7.cpsr & ~0x1Fu) | M_USR);
	u32 a = start;
	if (w && l) arm7.r[rn] = final;
	for (int i = 0; i < 16; i++) {
		if (!(list & (1u << i))) continue;
		if (l) {
			u32 v = arm_read32(a);
			if (i == 15) {
				write_pc(v);
				if (s) set_mode(arm7.spsr);
			} else {
				arm7.r[i] = v;
			}
		} else {
			arm_write32(a, i == 15 ? arm7.r[15] + 8 : arm7.r[i]);
		}
		a += 4;
	}
	if (user_bank) set_mode(saved_mode);
	if (w && !l) arm7.r[rn] = final;
}

static void exception(u32 vector, u32 mode, u32 return_addr, int disable_fiq)
{
	u32 old = arm7.cpsr;
	set_mode((old & ~0x1Fu) | mode | I_FLAG | (disable_fiq ? F_FLAG : 0));
	arm7.spsr = old;
	arm7.r[14] = return_addr;
	arm7.r[15] = vector;
}

static void execute(u32 instr)
{
	if (!cond_pass(instr >> 28))
		return;
	u32 top = (instr >> 25) & 7;
	switch (top) {
	case 0:
		if ((instr & 0x0FC000F0u) == 0x00000090u || (instr & 0x0F8000F0u) == 0x00800090u) { multiply(instr); return; }
		if ((instr & 0x0FB00FF0u) == 0x01000090u) {             /* SWP / SWPB */
			int rn = (instr >> 16) & 15, rd = (instr >> 12) & 15, rm = instr & 15;
			u32 addr = arm7.r[rn];
			if ((instr >> 22) & 1) { u8 t = arm_read8(addr); arm_write8(addr, (u8)arm7.r[rm]); arm7.r[rd] = t; }
			else { u32 t = load32(addr); arm_write32(addr & ~3u, arm7.r[rm]); arm7.r[rd] = t; }
			return;
		}
		if ((instr & 0x0E000090u) == 0x00000090u && (instr & 0x60)) { halfword_transfer(instr); return; }
		if ((instr & 0x0FBF0FFFu) == 0x010F0000u) {             /* MRS */
			arm7.r[(instr >> 12) & 15] = (instr >> 22) & 1 ? arm7.spsr : arm7.cpsr;
			return;
		}
		if ((instr & 0x0FB0FFF0u) == 0x0120F000u) goto msr;     /* MSR register */
		if ((instr & 0x0FFFFFF0u) == 0x012FFF10u) {             /* BX (ARMv4T; treat as plain branch) */
			write_pc(arm7.r[instr & 15]);
			return;
		}
		data_processing(instr);
		return;
	case 1:
		if ((instr & 0x0FB0F000u) == 0x0320F000u) goto msr;     /* MSR immediate */
		data_processing(instr);
		return;
	case 2: case 3:
		single_transfer(instr);
		return;
	case 4:
		block_transfer(instr);
		return;
	case 5: {
		s32 off = (s32)(instr << 8) >> 6;
		if ((instr >> 24) & 1) arm7.r[14] = arm7.r[15];          /* return to the next instruction */
		arm7.r[15] = arm7.r[15] + 4 + (u32)off;
		return;
	}
	case 7:
		if ((instr >> 24) & 1) {                               /* SWI */
			exception(0x08, M_SVC, arm7.r[15], 0);
			return;
		}
		return;                                                /* coprocessor: none */
	default:
		return;
	}
msr: {
		u32 v;
		if (instr & (1u << 25)) {
			u32 imm = instr & 0xFF, rot = ((instr >> 8) & 15) * 2;
			v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
		} else {
			v = arm7.r[instr & 15];
		}
		u32 mask = 0;
		if (instr & (1u << 16)) mask |= 0x000000FFu;
		if (instr & (1u << 19)) mask |= 0xFF000000u;
		if ((instr >> 22) & 1) {
			arm7.spsr = (arm7.spsr & ~mask) | (v & mask);
		} else {
			if ((arm7.cpsr & 0x1F) == M_USR) mask &= 0xFF000000u;   /* user mode: flags only */
			u32 n = (arm7.cpsr & ~mask) | (v & mask);
			set_mode(n);
		}
	}
}

void arm7_run(int cycles)
{
	for (int i = 0; i < cycles; i++) {
		if (arm7.fiq_line && !(arm7.cpsr & F_FLAG)) {
			exception(0x1C, M_FIQ, arm7.r[15] + 4, 1);
			arm7_fiqs++;
		}
		u32 pc = arm7.r[15];
		u32 instr = arm_read32(pc);
		arm7.r[15] = pc + 4;       /* next instruction; branches overwrite it */
		execute(instr);
		arm7.cycles++;
	}
}
