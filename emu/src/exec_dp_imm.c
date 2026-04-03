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

static int
exec_pc_rel(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	rd;
	uint64_t	imm;
	int		op;

	rd = bits(insn, 4, 0);
	op = bit(insn, 31);

	/* immhi = bits[23:5], immlo = bits[30:29] */
	imm = (uint64_t)(((uint64_t)bits(insn, 23, 5) << 2) |
	    (uint64_t)bits(insn, 30, 29));

	if (op == 0) {
		/* ADR: rd = pc + sign_extend(imm, 21) */
		imm = (uint64_t)sign_extend(imm, 21);
		cpu_set_xreg(cpu, rd, cpu->pc + imm);
	} else {
		/* ADRP: rd = (pc & ~0xFFF) + sign_extend(imm, 21) << 12 */
		imm = (uint64_t)sign_extend(imm, 21);
		cpu_set_xreg(cpu, rd,
		    (cpu->pc & ~0xFFFULL) + (imm << 12));
	}

	return EMU_OK;
}

static int
exec_add_sub_imm(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, op, S, sh;
	uint32_t	imm12, rn, rd;
	uint64_t	imm, operand1, result;

	sf = bit(insn, 31);
	op = bit(insn, 30);
	S = bit(insn, 29);
	sh = bit(insn, 22);
	imm12 = bits(insn, 21, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	imm = (uint64_t)imm12;
	if (sh)
		imm <<= 12;

	/* rn always uses SP encoding */
	operand1 = cpu_xreg_sp(cpu, rn);

	if (sf) {
		/* 64-bit */
		if (op == 0) {
			result = operand1 + imm;
			if (S)
				cpu_update_flags_add64(cpu, operand1, imm,
				    result);
		} else {
			result = operand1 - imm;
			if (S)
				cpu_update_flags_sub64(cpu, operand1, imm,
				    result);
		}
		/* S=1: rd is ZR (flags-only = CMP/CMN), else rd is SP */
		if (S)
			cpu_set_xreg(cpu, rd, result);
		else
			cpu_set_xreg_sp(cpu, rd, result);
	} else {
		/* 32-bit */
		uint32_t a, b, r;

		a = (uint32_t)operand1;
		b = (uint32_t)imm;
		if (op == 0) {
			r = a + b;
			if (S)
				cpu_update_flags_add32(cpu, a, b, r);
		} else {
			r = a - b;
			if (S)
				cpu_update_flags_sub32(cpu, a, b, r);
		}
		if (S)
			cpu_set_wreg(cpu, rd, r);
		else
			cpu_set_xreg_sp(cpu, rd, (uint64_t)r);
	}

	return EMU_OK;
}

static int
exec_logical_imm(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, opc, N;
	uint32_t	immr, imms, rn, rd;
	uint64_t	imm_val, operand1, result;

	sf = bit(insn, 31);
	opc = bits(insn, 30, 29);
	N = bit(insn, 22);
	immr = bits(insn, 21, 16);
	imms = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (decode_bitmask_imm(sf, N, (int)immr, (int)imms, &imm_val) != 0) {
		LOG_WARN("invalid logical immediate at 0x%llx",
		    (unsigned long long)cpu->pc);
		return EMU_ERR;
	}

	operand1 = cpu_xreg(cpu, rn);
	if (!sf)
		operand1 &= 0xFFFFFFFF;

	switch (opc) {
	case 0:	/* AND */
		result = operand1 & imm_val;
		break;
	case 1:	/* ORR */
		result = operand1 | imm_val;
		break;
	case 2:	/* EOR */
		result = operand1 ^ imm_val;
		break;
	case 3:	/* ANDS */
		result = operand1 & imm_val;
		break;
	default:
		return EMU_ERR;
	}

	if (!sf)
		result &= 0xFFFFFFFF;

	if (opc == 3) {
		/* ANDS: update flags, rd uses ZR */
		if (sf)
			cpu_update_flags_nz64(cpu, result);
		else
			cpu_update_flags_nz32(cpu, (uint32_t)result);
		cpu_set_xreg(cpu, rd, result);
	} else {
		/* AND/ORR/EOR: rd uses SP */
		if (sf)
			cpu_set_xreg_sp(cpu, rd, result);
		else
			cpu_set_xreg_sp(cpu, rd, result & 0xFFFFFFFF);
	}

	return EMU_OK;
}

static int
exec_move_wide(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, opc;
	uint32_t	hw, rd;
	uint64_t	imm16, shift, val;

	sf = bit(insn, 31);
	opc = bits(insn, 30, 29);
	hw = bits(insn, 22, 21);
	imm16 = (uint64_t)bits(insn, 20, 5);
	rd = bits(insn, 4, 0);

	/* hw[1] must be 0 for 32-bit variant */
	if (!sf && (hw & 2))
		return EMU_ERR;

	shift = (uint64_t)hw * 16;

	switch (opc) {
	case 0:	/* MOVN */
		val = ~(imm16 << shift);
		if (!sf)
			val &= 0xFFFFFFFF;
		break;
	case 2:	/* MOVZ */
		val = imm16 << shift;
		break;
	case 3:	/* MOVK */
		val = cpu_xreg(cpu, rd);
		val &= ~(0xFFFFULL << shift);
		val |= imm16 << shift;
		if (!sf)
			val &= 0xFFFFFFFF;
		break;
	default:
		return EMU_UNIMPL;
	}

	if (sf)
		cpu_set_xreg(cpu, rd, val);
	else
		cpu_set_wreg(cpu, rd, (uint32_t)val);

	return EMU_OK;
}

static int
exec_bitfield(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, opc;
	uint32_t	immr, imms, rn, rd;
	uint64_t	src, dst, result;
	int		regwidth, wmask_len, pos;

	sf = bit(insn, 31);
	opc = bits(insn, 30, 29);
	immr = bits(insn, 21, 16);
	imms = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	regwidth = sf ? 64 : 32;
	src = cpu_xreg(cpu, rn);
	if (!sf)
		src &= 0xFFFFFFFF;

	switch (opc) {
	case 0:	/* SBFM */
		if (imms >= immr) {
			/* Extract bitfield: bits[imms:immr], sign-extend */
			wmask_len = (int)(imms - immr + 1);
			result = (src >> immr) & ((1ULL << wmask_len) - 1);
			result = (uint64_t)sign_extend(result, wmask_len);
		} else {
			/* Insert at high position, sign-extend */
			wmask_len = (int)(imms + 1);
			pos = regwidth - (int)immr;
			result = (src & ((1ULL << wmask_len) - 1)) <<
			    pos;
			result = (uint64_t)sign_extend(result,
			    wmask_len + pos);
		}
		if (!sf)
			result &= 0xFFFFFFFF;
		cpu_set_xreg(cpu, rd, result);
		break;

	case 1:	/* BFM */
		dst = cpu_xreg(cpu, rd);
		if (!sf)
			dst &= 0xFFFFFFFF;

		if (imms >= immr) {
			/* BFXIL: extract from rn, insert at lsb of rd */
			uint64_t mask;

			wmask_len = (int)(imms - immr + 1);
			mask = (1ULL << wmask_len) - 1;
			result = (dst & ~mask) |
			    ((src >> immr) & mask);
		} else {
			/* BFI: extract low bits of rn, insert into rd */
			uint64_t mask;

			wmask_len = (int)(imms + 1);
			pos = regwidth - (int)immr;
			mask = ((1ULL << wmask_len) - 1) << pos;
			result = (dst & ~mask) |
			    ((src & ((1ULL << wmask_len) - 1)) << pos);
		}
		if (!sf)
			result &= 0xFFFFFFFF;
		cpu_set_xreg(cpu, rd, result);
		break;

	case 2:	/* UBFM */
		if (imms >= immr) {
			/* Extract bitfield: bits[imms:immr], zero-extend */
			wmask_len = (int)(imms - immr + 1);
			result = (src >> immr) & ((1ULL << wmask_len) - 1);
		} else {
			/* Shift left and mask */
			wmask_len = (int)(imms + 1);
			pos = regwidth - (int)immr;
			result = (src & ((1ULL << wmask_len) - 1)) <<
			    pos;
		}
		if (!sf)
			result &= 0xFFFFFFFF;
		cpu_set_xreg(cpu, rd, result);
		break;

	default:
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

static int
exec_extract(cpu_state_t *cpu, uint32_t insn)
{
	int		sf;
	uint32_t	rm, imms, rn, rd;
	uint64_t	lo, hi, result;
	int		regwidth;

	sf = bit(insn, 31);
	rm = bits(insn, 20, 16);
	imms = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	regwidth = sf ? 64 : 32;

	hi = cpu_xreg(cpu, rn);
	lo = cpu_xreg(cpu, rm);

	if (!sf) {
		hi &= 0xFFFFFFFF;
		lo &= 0xFFFFFFFF;
	}

	if (imms == 0) {
		result = hi;
	} else {
		result = (lo >> imms) | (hi << (regwidth - imms));
	}

	if (!sf)
		result &= 0xFFFFFFFF;

	cpu_set_xreg(cpu, rd, result);
	return EMU_OK;
}

int
exec_dp_imm(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	op0;

	op0 = bits(insn, 25, 23);

	switch (op0) {
	case 0:
	case 1:	/* PC-relative addressing (bits[25:23] = 00x) */
		return exec_pc_rel(cpu, insn);
	case 2:
	case 3:	/* Add/subtract immediate (bits[25:23] = 01x) */
		return exec_add_sub_imm(cpu, insn);
	case 4:	/* Logical immediate (bits[25:23] = 100) */
		return exec_logical_imm(cpu, insn);
	case 5:	/* Move wide immediate (bits[25:23] = 101) */
		return exec_move_wide(cpu, insn);
	case 6:	/* Bitfield (bits[25:23] = 110) */
		return exec_bitfield(cpu, insn);
	case 7:	/* Extract (bits[25:23] = 111) */
		return exec_extract(cpu, insn);
	default:
		LOG_WARN("unimplemented dp_imm op0=%u at 0x%llx: 0x%08x",
		    op0, (unsigned long long)cpu->pc, insn);
		return EMU_UNIMPL;
	}
}
