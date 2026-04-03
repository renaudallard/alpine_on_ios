/*
 * Copyright (c) 2026 Alpine on iOS contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _POSIX_C_SOURCE	199309L
#include <time.h>

#include "cpu.h"
#include "decoder.h"
#include "emu.h"
#include "log.h"

/* Encode system register fields into a single key for switching */
#define SYSREG(op0, op1, crn, crm, op2) \
	(((op0) << 16) | ((op1) << 12) | ((crn) << 8) | ((crm) << 4) | (op2))

/* FPCR encoding: op0=3, op1=3, CRn=4, CRm=4, op2=0 */
#define SYSREG_FPCR	SYSREG(3, 3, 4, 4, 0)
/* FPSR encoding: op0=3, op1=3, CRn=4, CRm=4, op2=1 */
#define SYSREG_FPSR	SYSREG(3, 3, 4, 4, 1)

static uint64_t
read_counter(void)
{
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	/* Convert to 24MHz ticks */
	return (uint64_t)ts.tv_sec * 24000000ULL +
	    (uint64_t)ts.tv_nsec * 24ULL / 1000ULL;
}

static int
exec_mrs(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	op0, op1, crn, crm, op2, rt;
	uint32_t	key;
	uint64_t	val;

	op0 = bits(insn, 20, 19);
	op1 = bits(insn, 18, 16);
	crn = bits(insn, 15, 12);
	crm = bits(insn, 11, 8);
	op2 = bits(insn, 7, 5);
	rt = bits(insn, 4, 0);

	key = SYSREG(op0, op1, crn, crm, op2);

	switch (key) {
	case SYSREG(3, 3, 13, 0, 2):	/* TPIDR_EL0 */
		val = cpu->tpidr_el0;
		break;
	case SYSREG(3, 3, 13, 0, 3):	/* TPIDRRO_EL0 */
		val = cpu->tpidrro_el0;
		break;
	case SYSREG(3, 3, 14, 0, 0):	/* CNTFRQ_EL0 */
		val = 24000000;
		break;
	case SYSREG(3, 3, 14, 0, 1):	/* CNTVCT_EL0 */
		val = read_counter();
		break;
	case SYSREG(3, 3, 0, 0, 1):	/* CTR_EL0 */
		val = 0x84448004;
		break;
	case SYSREG(3, 3, 0, 0, 7):	/* DCZID_EL0 */
		val = 0x4;
		break;
	case SYSREG(3, 0, 0, 0, 0):	/* MIDR_EL1 */
		val = 0x410FD034;
		break;
	case SYSREG_FPCR:
		val = (uint64_t)cpu->fpcr;
		break;
	case SYSREG_FPSR:
		val = (uint64_t)cpu->fpsr;
		break;
	case SYSREG(3, 0, 0, 0, 5):	/* MPIDR_EL1 */
		val = 0x80000000;		/* single core */
		break;
	case SYSREG(3, 0, 0, 0, 6):	/* REVIDR_EL1 */
		val = 0;
		break;
	default:
		LOG_WARN("MRS unknown sysreg op0=%u op1=%u CRn=%u CRm=%u "
		    "op2=%u at 0x%llx", op0, op1, crn, crm, op2,
		    (unsigned long long)cpu->pc);
		val = 0;
		break;
	}

	cpu_set_xreg(cpu, rt, val);
	return EMU_OK;
}

static int
exec_msr(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	op0, op1, crn, crm, op2, rt;
	uint32_t	key;
	uint64_t	val;

	op0 = bits(insn, 20, 19);
	op1 = bits(insn, 18, 16);
	crn = bits(insn, 15, 12);
	crm = bits(insn, 11, 8);
	op2 = bits(insn, 7, 5);
	rt = bits(insn, 4, 0);

	val = cpu_xreg(cpu, rt);
	key = SYSREG(op0, op1, crn, crm, op2);

	switch (key) {
	case SYSREG(3, 3, 13, 0, 2):	/* TPIDR_EL0 */
		cpu->tpidr_el0 = val;
		break;
	case SYSREG_FPCR:
		cpu->fpcr = (uint32_t)val;
		break;
	case SYSREG_FPSR:
		cpu->fpsr = (uint32_t)val;
		break;
	default:
		LOG_WARN("MSR unknown sysreg op0=%u op1=%u CRn=%u CRm=%u "
		    "op2=%u at 0x%llx", op0, op1, crn, crm, op2,
		    (unsigned long long)cpu->pc);
		break;
	}

	return EMU_OK;
}

int
exec_system(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	L;

	/* NOP and hint instructions: 0xD503201F and similar */
	if (insn == 0xD503201F)
		return EMU_OK;		/* NOP */

	/* YIELD, WFE, WFI, SEV, SEVL - treat as NOP */
	if ((insn & 0xFFFFF0FF) == 0xD503201F)
		return EMU_OK;

	/* Hint instructions (including NOP variants) */
	if ((insn & 0xFFFFF01F) == 0xD503201F)
		return EMU_OK;

	/* Barriers: DMB, DSB, ISB */
	if ((insn & 0xFFFFF000) == 0xD5033000)
		return EMU_OK;		/* DSB variants */
	if ((insn & 0xFFFFF000) == 0xD5032000)
		return EMU_OK;		/* CLREX */
	if ((insn & 0xFFFFF0FF) == 0xD50330BF)
		return EMU_OK;		/* DMB */
	if ((insn & 0xFFFFF0FF) == 0xD50330DF)
		return EMU_OK;		/* ISB */

	/* SYS / SYSL instructions (DC, IC, etc.) - treat as NOP */
	if ((insn & 0xFFF80000) == 0xD5080000)
		return EMU_OK;

	/* MRS / MSR */
	L = bit(insn, 21);

	if (bit(insn, 20)) {
		/* op0 >= 2: system register access */
		if (L)
			return exec_mrs(cpu, insn);
		else
			return exec_msr(cpu, insn);
	}

	LOG_WARN("unimplemented system insn at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
