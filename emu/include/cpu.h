/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <string.h>

#include "memory.h"

/*
 * Software TLB for the interpreter.  Direct-mapped, indexed by
 * guest page number.  Each entry caches the host pointer for
 * one 4K guest page so mem_translate can skip the region walk
 * and lock on a hit.
 */
#define TLB_BITS	4
#define TLB_SIZE	(1 << TLB_BITS)
#define TLB_PAGE_SHIFT	12
#define TLB_PAGE_MASK	(~((uint64_t)(1 << TLB_PAGE_SHIFT) - 1))

typedef struct {
	uint64_t	tag;	/* guest page (addr & TLB_PAGE_MASK) */
	uint8_t		*host;	/* host base of this page */
	int		prot;	/* cached protection bits */
} tlb_entry_t;

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

	/* Software TLB for interpreter fast path */
	tlb_entry_t	tlb[TLB_SIZE];

	/* Native host register save area (x19-x30, sp, padding) */
	uint64_t	native_host_save[14];
} cpu_state_t;

/* Flush all TLB entries (call after mmap/munmap/mprotect). */
static inline void
cpu_tlb_flush(cpu_state_t *cpu)
{
	for (int i = 0; i < TLB_SIZE; i++)
		cpu->tlb[i].tag = ~(uint64_t)0;
}

/*
 * Fast TLB lookup.  Returns host pointer on hit, NULL on miss.
 * Caller must fall back to mem_translate on miss.
 */
static inline void *
cpu_tlb_lookup(cpu_state_t *cpu, uint64_t addr, uint64_t size, int prot)
{
	uint64_t	 page;
	unsigned	 idx;
	tlb_entry_t	*e;

	page = addr & TLB_PAGE_MASK;
	/* Check access doesn't cross page boundary. */
	if (__builtin_expect(((addr + size - 1) & TLB_PAGE_MASK) != page, 0))
		return NULL;

	idx = (unsigned)(page >> TLB_PAGE_SHIFT) & (TLB_SIZE - 1);
	e = &cpu->tlb[idx];

	if (__builtin_expect(e->tag == page && (e->prot & prot) == prot, 1))
		return e->host + (addr - page);

	return NULL;
}

/* Insert a TLB entry after a mem_translate hit. */
static inline void
cpu_tlb_insert(cpu_state_t *cpu, uint64_t addr, void *host, int prot)
{
	uint64_t	page;
	unsigned	idx;

	page = addr & TLB_PAGE_MASK;
	idx = (unsigned)(page >> TLB_PAGE_SHIFT) & (TLB_SIZE - 1);

	cpu->tlb[idx].tag = page;
	cpu->tlb[idx].host = (uint8_t *)host - (addr - page);
	cpu->tlb[idx].prot = prot;
}

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

/*
 * TLB-accelerated memory access for the interpreter hot path.
 * Falls back to mem_translate (with rwlock) on TLB miss, then
 * populates the TLB entry for subsequent hits.
 */
static inline int
cpu_mem_read32(cpu_state_t *cpu, uint64_t addr, uint32_t *val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 4, MEM_PROT_READ);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 4, MEM_PROT_READ);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_READ);
	}
	memcpy(val, p, 4);
	return 0;
}

static inline int
cpu_mem_read8(cpu_state_t *cpu, uint64_t addr, uint8_t *val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 1, MEM_PROT_READ);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 1, MEM_PROT_READ);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_READ);
	}
	*val = *(uint8_t *)p;
	return 0;
}

static inline int
cpu_mem_read16(cpu_state_t *cpu, uint64_t addr, uint16_t *val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 2, MEM_PROT_READ);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 2, MEM_PROT_READ);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_READ);
	}
	memcpy(val, p, 2);
	return 0;
}

static inline int
cpu_mem_read64(cpu_state_t *cpu, uint64_t addr, uint64_t *val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 8, MEM_PROT_READ);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 8, MEM_PROT_READ);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_READ);
	}
	memcpy(val, p, 8);
	return 0;
}

static inline int
cpu_mem_write8(cpu_state_t *cpu, uint64_t addr, uint8_t val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 1, MEM_PROT_WRITE);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 1, MEM_PROT_WRITE);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_WRITE);
	}
	*(uint8_t *)p = val;
	return 0;
}

static inline int
cpu_mem_write16(cpu_state_t *cpu, uint64_t addr, uint16_t val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 2, MEM_PROT_WRITE);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 2, MEM_PROT_WRITE);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_WRITE);
	}
	memcpy(p, &val, 2);
	return 0;
}

static inline int
cpu_mem_write32(cpu_state_t *cpu, uint64_t addr, uint32_t val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 4, MEM_PROT_WRITE);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 4, MEM_PROT_WRITE);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_WRITE);
	}
	memcpy(p, &val, 4);
	return 0;
}

static inline int
cpu_mem_write64(cpu_state_t *cpu, uint64_t addr, uint64_t val)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, 8, MEM_PROT_WRITE);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, 8, MEM_PROT_WRITE);
		if (p == NULL)
			return -1;
		cpu_tlb_insert(cpu, addr, p, MEM_PROT_WRITE);
	}
	memcpy(p, &val, 8);
	return 0;
}

/* Host pointer via TLB, for multi-byte/SIMD access. */
static inline void *
cpu_mem_ptr(cpu_state_t *cpu, uint64_t addr, uint64_t size, int prot)
{
	void	*p;

	p = cpu_tlb_lookup(cpu, addr, size, prot);
	if (p == NULL) {
		p = mem_translate(cpu->mem, addr, size, prot);
		if (p == NULL)
			return NULL;
		cpu_tlb_insert(cpu, addr, p, prot);
	}
	return p;
}

#endif /* CPU_H */
