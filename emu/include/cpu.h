/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* Forward declaration */
typedef struct mem_space mem_space_t;

/* PSTATE condition flags */
#define PSTATE_N	(1u << 31)
#define PSTATE_Z	(1u << 30)
#define PSTATE_C	(1u << 29)
#define PSTATE_V	(1u << 28)

/* Condition codes */
#define COND_EQ		0
#define COND_NE		1
#define COND_CS		2
#define COND_CC		3
#define COND_MI		4
#define COND_PL		5
#define COND_VS		6
#define COND_VC		7
#define COND_HI		8
#define COND_LS		9
#define COND_GE		10
#define COND_LT		11
#define COND_GT		12
#define COND_LE		13
#define COND_AL		14
#define COND_NV		15

/* SIMD/FP register (128-bit) */
typedef union {
	uint8_t		b[16];
	uint16_t	h[8];
	uint32_t	s[4];
	uint64_t	d[2];
	float		sf[4];
	double		df[2];
} vreg_t;

/* AArch64 CPU state */
typedef struct cpu_state {
	uint64_t	x[31];		/* X0-X30 */
	uint64_t	sp;		/* Stack pointer */
	uint64_t	pc;		/* Program counter */
	uint32_t	nzcv;		/* Condition flags */

	vreg_t		v[32];		/* V0-V31 SIMD/FP registers */
	uint32_t	fpcr;		/* FP control register */
	uint32_t	fpsr;		/* FP status register */

	uint64_t	tpidr_el0;	/* User thread pointer */
	uint64_t	tpidrro_el0;	/* Read-only thread pointer */

	/* Exclusive monitor for LDXR/STXR */
	uint64_t	excl_addr;
	uint64_t	excl_val;
	int		excl_active;

	mem_space_t	*mem;
	int		running;
	int		exit_code;

	/* JIT host register save area (x19-x30, sp, padding) */
	uint64_t	jit_host_save[14];
} cpu_state_t;

void	cpu_init(cpu_state_t *cpu);
int	cpu_step(cpu_state_t *cpu);
int	cpu_check_cond(cpu_state_t *cpu, unsigned int cond);

/* Register access: R31 = zero register */
static inline uint64_t
cpu_xreg(cpu_state_t *cpu, int reg)
{
	return reg == 31 ? 0 : cpu->x[reg];
}

static inline uint32_t
cpu_wreg(cpu_state_t *cpu, int reg)
{
	return reg == 31 ? 0 : (uint32_t)cpu->x[reg];
}

/* Register access: R31 = stack pointer */
static inline uint64_t
cpu_xreg_sp(cpu_state_t *cpu, int reg)
{
	return reg == 31 ? cpu->sp : cpu->x[reg];
}

static inline void
cpu_set_xreg(cpu_state_t *cpu, int reg, uint64_t val)
{
	if (reg != 31)
		cpu->x[reg] = val;
}

static inline void
cpu_set_wreg(cpu_state_t *cpu, int reg, uint32_t val)
{
	if (reg != 31)
		cpu->x[reg] = (uint64_t)val;	/* zero-extend */
}

static inline void
cpu_set_xreg_sp(cpu_state_t *cpu, int reg, uint64_t val)
{
	if (reg == 31)
		cpu->sp = val;
	else
		cpu->x[reg] = val;
}

/* Bit extraction */
static inline uint32_t
bits(uint32_t val, int hi, int lo)
{
	return (val >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static inline uint32_t
bit(uint32_t val, int pos)
{
	return (val >> pos) & 1;
}

static inline int64_t
sign_extend(uint64_t val, int width)
{
	int shift = 64 - width;
	return (int64_t)(val << shift) >> shift;
}

/* Flag helpers */
void	cpu_update_flags_add32(cpu_state_t *, uint32_t, uint32_t, uint32_t);
void	cpu_update_flags_add64(cpu_state_t *, uint64_t, uint64_t, uint64_t);
void	cpu_update_flags_sub32(cpu_state_t *, uint32_t, uint32_t, uint32_t);
void	cpu_update_flags_sub64(cpu_state_t *, uint64_t, uint64_t, uint64_t);
void	cpu_update_flags_nz32(cpu_state_t *, uint32_t);
void	cpu_update_flags_nz64(cpu_state_t *, uint64_t);

/* Bitmask immediate decoder for logical instructions */
int	decode_bitmask_imm(int sf, int N, int immr, int imms, uint64_t *out);

#endif /* CPU_H */
