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

#include <string.h>

#include "cpu.h"
#include "decoder.h"
#include "memory.h"
#include "emu.h"
#include "log.h"

void
cpu_init(cpu_state_t *cpu)
{
	memset(cpu, 0, sizeof(*cpu));
}

int
cpu_step(cpu_state_t *cpu)
{
	uint32_t	insn;
	uint64_t	old_pc;
	int		group;
	int		rc;

	if (mem_read32(cpu->mem, cpu->pc, &insn) != 0) {
		LOG_ERR("instruction fetch fault at 0x%llx",
		    (unsigned long long)cpu->pc);
		return EMU_SEGFAULT;
	}

	old_pc = cpu->pc;

	group = (insn >> 25) & 0xF;

	switch (group) {
	case 0x8:
	case 0x9:
		rc = exec_dp_imm(cpu, insn);
		break;
	case 0xA:
	case 0xB:
		rc = exec_branch(cpu, insn);
		break;
	case 0x4:
	case 0x6:
	case 0xC:
	case 0xE:
		rc = exec_ldst(cpu, insn);
		break;
	case 0x5:
	case 0xD:
		rc = exec_dp_reg(cpu, insn);
		break;
	case 0x7:
	case 0xF:
		rc = exec_simd(cpu, insn);
		break;
	default:
		LOG_WARN("unimplemented insn group %d at 0x%llx: 0x%08x",
		    group, (unsigned long long)cpu->pc, insn);
		return EMU_UNIMPL;
	}

	/* Advance PC if instruction did not branch */
	if (rc == EMU_OK && cpu->pc == old_pc)
		cpu->pc += 4;

	return rc;
}

int
cpu_check_cond(cpu_state_t *cpu, unsigned int cond)
{
	uint32_t	flags;
	int		result;
	int		n, z, c, v;

	flags = cpu->nzcv;
	n = (flags >> 31) & 1;
	z = (flags >> 30) & 1;
	c = (flags >> 29) & 1;
	v = (flags >> 28) & 1;

	switch (cond >> 1) {
	case 0:	/* EQ/NE */
		result = z;
		break;
	case 1:	/* CS/CC */
		result = c;
		break;
	case 2:	/* MI/PL */
		result = n;
		break;
	case 3:	/* VS/VC */
		result = v;
		break;
	case 4:	/* HI/LS */
		result = c && !z;
		break;
	case 5:	/* GE/LT */
		result = (n == v);
		break;
	case 6:	/* GT/LE */
		result = !z && (n == v);
		break;
	case 7:	/* AL/NV */
		result = 1;
		break;
	default:
		result = 0;
		break;
	}

	/* Invert if low bit of condition is set (except AL/NV) */
	if ((cond & 1) && (cond != 14) && (cond != 15))
		result = !result;

	return result;
}

void
cpu_update_flags_add32(cpu_state_t *cpu, uint32_t a, uint32_t b,
    uint32_t result)
{
	uint32_t flags = 0;

	if (result & (1u << 31))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	if (result < a)
		flags |= PSTATE_C;
	if (((a ^ result) & (b ^ result)) & (1u << 31))
		flags |= PSTATE_V;
	cpu->nzcv = flags;
}

void
cpu_update_flags_add64(cpu_state_t *cpu, uint64_t a, uint64_t b,
    uint64_t result)
{
	uint32_t flags = 0;

	if (result & (1ULL << 63))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	if (result < a)
		flags |= PSTATE_C;
	if (((a ^ result) & (b ^ result)) & (1ULL << 63))
		flags |= PSTATE_V;
	cpu->nzcv = flags;
}

void
cpu_update_flags_sub32(cpu_state_t *cpu, uint32_t a, uint32_t b,
    uint32_t result)
{
	uint32_t flags = 0;

	if (result & (1u << 31))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	if (a >= b)
		flags |= PSTATE_C;
	if (((a ^ b) & (a ^ result)) & (1u << 31))
		flags |= PSTATE_V;
	cpu->nzcv = flags;
}

void
cpu_update_flags_sub64(cpu_state_t *cpu, uint64_t a, uint64_t b,
    uint64_t result)
{
	uint32_t flags = 0;

	if (result & (1ULL << 63))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	if (a >= b)
		flags |= PSTATE_C;
	if (((a ^ b) & (a ^ result)) & (1ULL << 63))
		flags |= PSTATE_V;
	cpu->nzcv = flags;
}

void
cpu_update_flags_nz32(cpu_state_t *cpu, uint32_t result)
{
	uint32_t flags = 0;

	if (result & (1u << 31))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	cpu->nzcv = flags;
}

void
cpu_update_flags_nz64(cpu_state_t *cpu, uint64_t result)
{
	uint32_t flags = 0;

	if (result & (1ULL << 63))
		flags |= PSTATE_N;
	if (result == 0)
		flags |= PSTATE_Z;
	cpu->nzcv = flags;
}

/*
 * Decode AArch64 logical immediate.
 * Returns 0 on success, -1 if the encoding is reserved.
 */
int
decode_bitmask_imm(int sf, int N, int immr, int imms, uint64_t *out)
{
	uint64_t	mask;
	uint64_t	welem;
	int		len, esize, levels;
	int		S, R;
	int		i;
	uint32_t	combined;

	/* Find highest set bit in N:~imms */
	combined = (uint32_t)((N << 6) | (~imms & 0x3F));

	/* clz-based highest_set_bit */
	if (combined == 0)
		return -1;

	len = 0;
	for (i = 6; i >= 0; i--) {
		if (combined & (1u << i)) {
			len = i;
			break;
		}
	}

	if (len < 1)
		return -1;

	esize = 1 << len;
	levels = esize - 1;

	/* Check for reserved value */
	if ((imms & levels) == levels)
		return -1;

	/* Must be valid for register width */
	if (!sf && N)
		return -1;

	S = imms & levels;
	R = immr & levels;

	/* Create the element value: S+1 ones */
	welem = (1ULL << (S + 1)) - 1;

	/* Rotate right within element size */
	if (R != 0) {
		uint64_t emask;

		emask = (esize == 64) ? ~0ULL : (1ULL << esize) - 1;
		welem = ((welem >> R) | (welem << (esize - R))) & emask;
	}

	/* Replicate to fill 64 bits */
	mask = welem;
	for (i = esize; i < 64; i <<= 1)
		mask |= mask << i;

	if (!sf)
		mask &= 0xFFFFFFFF;

	*out = mask;
	return 0;
}
