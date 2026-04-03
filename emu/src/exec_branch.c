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

#include "cpu.h"
#include "decoder.h"
#include "emu.h"
#include "log.h"

int
exec_branch(cpu_state_t *cpu, uint32_t insn)
{
	/* Unconditional branch immediate: (insn & 0x7C000000) == 0x14000000 */
	if ((insn & 0x7C000000) == 0x14000000) {
		int64_t	offset;
		int	op;

		op = bit(insn, 31);	/* 0=B, 1=BL */
		offset = sign_extend(bits(insn, 25, 0), 26) << 2;

		if (op)
			cpu->x[30] = cpu->pc + 4;	/* BL: link */

		cpu->pc = (uint64_t)((int64_t)cpu->pc + offset);
		return EMU_OK;
	}

	/* Conditional branch: (insn & 0xFF000010) == 0x54000000 */
	if ((insn & 0xFF000010) == 0x54000000) {
		uint32_t	cond;
		int64_t		offset;

		cond = bits(insn, 3, 0);
		offset = sign_extend(bits(insn, 23, 5), 19) << 2;

		if (cpu_check_cond(cpu, cond))
			cpu->pc = (uint64_t)((int64_t)cpu->pc + offset);
		else
			cpu->pc += 4;
		return EMU_OK;
	}

	/* Compare and branch: (insn & 0x7E000000) == 0x34000000 */
	if ((insn & 0x7E000000) == 0x34000000) {
		int		sf, op;
		uint32_t	rt;
		int64_t		offset;
		uint64_t	val;

		sf = bit(insn, 31);
		op = bit(insn, 24);	/* 0=CBZ, 1=CBNZ */
		offset = sign_extend(bits(insn, 23, 5), 19) << 2;
		rt = bits(insn, 4, 0);

		if (sf)
			val = cpu_xreg(cpu, rt);
		else
			val = (uint64_t)cpu_wreg(cpu, rt);

		if ((op == 0 && val == 0) || (op == 1 && val != 0))
			cpu->pc = (uint64_t)((int64_t)cpu->pc + offset);
		else
			cpu->pc += 4;
		return EMU_OK;
	}

	/* Test and branch: (insn & 0x7E000000) == 0x36000000 */
	if ((insn & 0x7E000000) == 0x36000000) {
		int		op;
		uint32_t	b5, b40, rt;
		int64_t		offset;
		int		bitpos;
		uint64_t	val;

		b5 = bit(insn, 31);
		op = bit(insn, 24);	/* 0=TBZ, 1=TBNZ */
		b40 = bits(insn, 23, 19);
		offset = sign_extend(bits(insn, 18, 5), 14) << 2;
		rt = bits(insn, 4, 0);

		bitpos = (int)((b5 << 5) | b40);
		val = cpu_xreg(cpu, rt);

		if ((op == 0 && !(val & (1ULL << bitpos))) ||
		    (op == 1 && (val & (1ULL << bitpos))))
			cpu->pc = (uint64_t)((int64_t)cpu->pc + offset);
		else
			cpu->pc += 4;
		return EMU_OK;
	}

	/* Exception generation: (insn & 0xFF000000) == 0xD4000000 */
	if ((insn & 0xFF000000) == 0xD4000000) {
		uint32_t	opc, ll;

		opc = bits(insn, 23, 21);
		ll = bits(insn, 1, 0);

		if (opc == 0 && ll == 1) {
			/* SVC */
			cpu->pc += 4;	/* advance past SVC */
			return EMU_SYSCALL;
		}
		if (opc == 1 && ll == 0) {
			/* BRK */
			return EMU_BREAK;
		}

		LOG_WARN("unimplemented exception opc=%u ll=%u at 0x%llx",
		    opc, ll, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	/* Unconditional branch register: (insn & 0xFE000000) == 0xD6000000 */
	if ((insn & 0xFE000000) == 0xD6000000) {
		uint32_t	opc, rn;
		uint64_t	target;

		opc = bits(insn, 24, 21);
		rn = bits(insn, 9, 5);
		target = cpu_xreg(cpu, rn);

		switch (opc) {
		case 0:	/* BR */
			cpu->pc = target;
			return EMU_OK;
		case 1:	/* BLR */
			cpu->x[30] = cpu->pc + 4;
			cpu->pc = target;
			return EMU_OK;
		case 2:	/* RET */
			cpu->pc = target;
			return EMU_OK;
		default:
			LOG_WARN("unimplemented br_reg opc=%u at 0x%llx",
			    opc, (unsigned long long)cpu->pc);
			return EMU_UNIMPL;
		}
	}

	/* System instructions: (insn & 0xFFC00000) == 0xD5000000 */
	if ((insn & 0xFFC00000) == 0xD5000000)
		return exec_system(cpu, insn);

	LOG_WARN("unimplemented branch/system at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
