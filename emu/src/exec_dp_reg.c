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

/* From decoder.c */
uint64_t	apply_shift(uint64_t, int, int);
uint32_t	apply_shift32(uint32_t, int, int);
uint64_t	extend_reg(cpu_state_t *, int, int, int);

static int
exec_logical_shifted(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, opc, shift_type, N;
	uint32_t	rm, imm6, rn, rd;
	uint64_t	operand1, operand2, result;

	sf = bit(insn, 31);
	opc = bits(insn, 30, 29);
	shift_type = bits(insn, 23, 22);
	N = bit(insn, 21);
	rm = bits(insn, 20, 16);
	imm6 = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (!sf && (imm6 & 0x20))
		return EMU_ERR;

	if (sf) {
		operand1 = cpu_xreg(cpu, rn);
		operand2 = apply_shift(cpu_xreg(cpu, rm), shift_type,
		    (int)imm6);
	} else {
		operand1 = (uint64_t)cpu_wreg(cpu, rn);
		operand2 = (uint64_t)apply_shift32(cpu_wreg(cpu, rm),
		    shift_type, (int)imm6);
	}

	if (N)
		operand2 = ~operand2;

	switch (opc) {
	case 0:	/* AND / BIC */
		result = operand1 & operand2;
		break;
	case 1:	/* ORR / ORN */
		result = operand1 | operand2;
		break;
	case 2:	/* EOR / EON */
		result = operand1 ^ operand2;
		break;
	case 3:	/* ANDS / BICS */
		result = operand1 & operand2;
		break;
	default:
		return EMU_ERR;
	}

	if (sf) {
		if (opc == 3)
			cpu_update_flags_nz64(cpu, result);
		cpu_set_xreg(cpu, rd, result);
	} else {
		result &= 0xFFFFFFFF;
		if (opc == 3)
			cpu_update_flags_nz32(cpu, (uint32_t)result);
		cpu_set_wreg(cpu, rd, (uint32_t)result);
	}

	return EMU_OK;
}

static int
exec_add_sub_shifted(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, op, S, shift_type;
	uint32_t	rm, imm6, rn, rd;
	uint64_t	operand1, operand2, result;

	sf = bit(insn, 31);
	op = bit(insn, 30);
	S = bit(insn, 29);
	shift_type = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	imm6 = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (shift_type == 3)
		return EMU_ERR;
	if (!sf && (imm6 & 0x20))
		return EMU_ERR;

	if (sf) {
		operand1 = cpu_xreg(cpu, rn);
		operand2 = apply_shift(cpu_xreg(cpu, rm), shift_type,
		    (int)imm6);

		if (op == 0) {
			result = operand1 + operand2;
			if (S)
				cpu_update_flags_add64(cpu, operand1,
				    operand2, result);
		} else {
			result = operand1 - operand2;
			if (S)
				cpu_update_flags_sub64(cpu, operand1,
				    operand2, result);
		}

		if (S)
			cpu_set_xreg(cpu, rd, result);
		else
			cpu_set_xreg(cpu, rd, result);
	} else {
		uint32_t a, b, r;

		a = cpu_wreg(cpu, rn);
		b = apply_shift32(cpu_wreg(cpu, rm), shift_type, (int)imm6);

		if (op == 0) {
			r = a + b;
			if (S)
				cpu_update_flags_add32(cpu, a, b, r);
		} else {
			r = a - b;
			if (S)
				cpu_update_flags_sub32(cpu, a, b, r);
		}

		cpu_set_wreg(cpu, rd, r);
	}

	return EMU_OK;
}

static int
exec_add_sub_extended(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, op, S;
	uint32_t	rm, option, imm3, rn, rd;
	uint64_t	operand1, operand2, result;

	sf = bit(insn, 31);
	op = bit(insn, 30);
	S = bit(insn, 29);
	rm = bits(insn, 20, 16);
	option = bits(insn, 15, 13);
	imm3 = bits(insn, 12, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (imm3 > 4)
		return EMU_ERR;

	operand1 = cpu_xreg_sp(cpu, rn);
	operand2 = extend_reg(cpu, (int)rm, (int)option, (int)imm3);

	if (sf) {
		if (op == 0) {
			result = operand1 + operand2;
			if (S)
				cpu_update_flags_add64(cpu, operand1,
				    operand2, result);
		} else {
			result = operand1 - operand2;
			if (S)
				cpu_update_flags_sub64(cpu, operand1,
				    operand2, result);
		}

		if (S)
			cpu_set_xreg(cpu, rd, result);
		else
			cpu_set_xreg_sp(cpu, rd, result);
	} else {
		uint32_t a, b, r;

		a = (uint32_t)operand1;
		b = (uint32_t)operand2;

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
exec_cond_compare(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, op, S, o2, o3;
	uint32_t	rn, nzcv_imm, cond;
	int		is_imm;
	uint64_t	operand1, operand2;

	sf = bit(insn, 31);
	op = bit(insn, 30);
	S = bit(insn, 29);
	rn = bits(insn, 9, 5);
	nzcv_imm = bits(insn, 3, 0);
	cond = bits(insn, 15, 12);
	o2 = bit(insn, 10);
	o3 = bit(insn, 4);
	is_imm = bit(insn, 11);

	if (S != 1 || o2 != 0 || o3 != 0)
		return EMU_ERR;

	if (cpu_check_cond(cpu, cond)) {
		if (is_imm) {
			operand2 = (uint64_t)bits(insn, 20, 16);
		} else {
			uint32_t rm = bits(insn, 20, 16);
			operand2 = cpu_xreg(cpu, rm);
		}

		operand1 = cpu_xreg(cpu, rn);

		if (sf) {
			if (op == 0) {
				/* CCMN: compare negative (add) */
				uint64_t r = operand1 + operand2;
				cpu_update_flags_add64(cpu, operand1,
				    operand2, r);
			} else {
				/* CCMP: compare (sub) */
				uint64_t r = operand1 - operand2;
				cpu_update_flags_sub64(cpu, operand1,
				    operand2, r);
			}
		} else {
			uint32_t a = (uint32_t)operand1;
			uint32_t b = (uint32_t)operand2;
			if (op == 0) {
				uint32_t r = a + b;
				cpu_update_flags_add32(cpu, a, b, r);
			} else {
				uint32_t r = a - b;
				cpu_update_flags_sub32(cpu, a, b, r);
			}
		}
	} else {
		cpu->nzcv = nzcv_imm << 28;
	}

	return EMU_OK;
}

static int
exec_cond_select(cpu_state_t *cpu, uint32_t insn)
{
	int		sf, op, op2;
	uint32_t	rm, cond, rn, rd;
	uint64_t	result;

	sf = bit(insn, 31);
	op = bit(insn, 30);
	rm = bits(insn, 20, 16);
	cond = bits(insn, 15, 12);
	op2 = bit(insn, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (cpu_check_cond(cpu, cond)) {
		result = cpu_xreg(cpu, rn);
	} else {
		result = cpu_xreg(cpu, rm);
		/* Apply modification based on op:op2 */
		switch ((op << 1) | op2) {
		case 0:	/* CSEL: no modification */
			break;
		case 1:	/* CSINC */
			result += 1;
			break;
		case 2:	/* CSINV */
			result = ~result;
			break;
		case 3:	/* CSNEG */
			result = (~result) + 1;
			break;
		}
	}

	if (sf)
		cpu_set_xreg(cpu, rd, result);
	else
		cpu_set_wreg(cpu, rd, (uint32_t)result);

	return EMU_OK;
}

static int
exec_dp_2src(cpu_state_t *cpu, uint32_t insn)
{
	int		sf;
	uint32_t	rm, opcode, rn, rd;
	uint64_t	n_val, m_val, result;

	sf = bit(insn, 31);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (sf) {
		n_val = cpu_xreg(cpu, rn);
		m_val = cpu_xreg(cpu, rm);
	} else {
		n_val = (uint64_t)cpu_wreg(cpu, rn);
		m_val = (uint64_t)cpu_wreg(cpu, rm);
	}

	switch (opcode) {
	case 2:	/* UDIV */
		if (m_val == 0)
			result = 0;
		else if (sf)
			result = n_val / m_val;
		else
			result = (uint64_t)((uint32_t)n_val /
			    (uint32_t)m_val);
		break;
	case 3:	/* SDIV */
		if (m_val == 0) {
			result = 0;
		} else if (sf) {
			int64_t sn = (int64_t)n_val;
			int64_t sm = (int64_t)m_val;
			if (sn == INT64_MIN && sm == -1)
				result = (uint64_t)INT64_MIN;
			else
				result = (uint64_t)(sn / sm);
		} else {
			int32_t sn = (int32_t)n_val;
			int32_t sm = (int32_t)m_val;
			if (sn == INT32_MIN && sm == -1)
				result = (uint64_t)(uint32_t)INT32_MIN;
			else
				result = (uint64_t)(uint32_t)(sn / sm);
		}
		break;
	case 8:	/* LSLV */
		if (sf) {
			int amt = (int)(m_val & 63);
			result = n_val << amt;
		} else {
			int amt = (int)(m_val & 31);
			result = (uint64_t)((uint32_t)n_val << amt);
		}
		break;
	case 9:	/* LSRV */
		if (sf) {
			int amt = (int)(m_val & 63);
			result = n_val >> amt;
		} else {
			int amt = (int)(m_val & 31);
			result = (uint64_t)((uint32_t)n_val >> amt);
		}
		break;
	case 10:	/* ASRV */
		if (sf) {
			int amt = (int)(m_val & 63);
			result = (uint64_t)((int64_t)n_val >> amt);
		} else {
			int amt = (int)(m_val & 31);
			result = (uint64_t)(uint32_t)(
			    (int32_t)n_val >> amt);
		}
		break;
	case 11:	/* RORV */
		if (sf) {
			int amt = (int)(m_val & 63);
			if (amt == 0)
				result = n_val;
			else
				result = (n_val >> amt) |
				    (n_val << (64 - amt));
		} else {
			int amt = (int)(m_val & 31);
			uint32_t v = (uint32_t)n_val;
			if (amt == 0)
				result = (uint64_t)v;
			else
				result = (uint64_t)((v >> amt) |
				    (v << (32 - amt)));
		}
		break;
	default:
		LOG_WARN("unimplemented dp_2src opcode=%u at 0x%llx",
		    opcode, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	if (sf)
		cpu_set_xreg(cpu, rd, result);
	else
		cpu_set_wreg(cpu, rd, (uint32_t)result);

	return EMU_OK;
}

static int
exec_dp_1src(cpu_state_t *cpu, uint32_t insn)
{
	int		sf;
	uint32_t	opcode, rn, rd;
	uint64_t	val, result;

	sf = bit(insn, 31);
	opcode = bits(insn, 15, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (sf)
		val = cpu_xreg(cpu, rn);
	else
		val = (uint64_t)cpu_wreg(cpu, rn);

	switch (opcode) {
	case 0: {	/* RBIT */
		uint64_t r = 0;
		int i;
		int width = sf ? 64 : 32;
		for (i = 0; i < width; i++) {
			if (val & (1ULL << i))
				r |= 1ULL << (width - 1 - i);
		}
		result = r;
		break;
	}
	case 1: {	/* REV16 */
		uint64_t r = 0;
		int i;
		int width = sf ? 8 : 4;
		for (i = 0; i < width; i += 2) {
			uint64_t b0 = (val >> (i * 8)) & 0xFF;
			uint64_t b1 = (val >> ((i + 1) * 8)) & 0xFF;
			r |= b1 << (i * 8);
			r |= b0 << ((i + 1) * 8);
		}
		result = r;
		break;
	}
	case 2:	/* REV32 (REV for 32-bit) */
		if (sf) {
			/* REV32: swap bytes within each 32-bit half */
			uint32_t lo = (uint32_t)val;
			uint32_t hi = (uint32_t)(val >> 32);
			lo = ((lo >> 24) & 0xFF) |
			    ((lo >> 8) & 0xFF00) |
			    ((lo << 8) & 0xFF0000) |
			    ((lo << 24) & 0xFF000000);
			hi = ((hi >> 24) & 0xFF) |
			    ((hi >> 8) & 0xFF00) |
			    ((hi << 8) & 0xFF0000) |
			    ((hi << 24) & 0xFF000000);
			result = ((uint64_t)hi << 32) | lo;
		} else {
			/* REV for W reg */
			uint32_t v = (uint32_t)val;
			result = (uint64_t)(
			    ((v >> 24) & 0xFF) |
			    ((v >> 8) & 0xFF00) |
			    ((v << 8) & 0xFF0000) |
			    ((v << 24) & 0xFF000000));
		}
		break;
	case 3:	/* REV (64-bit) */
		if (!sf)
			return EMU_ERR;
		result = ((val >> 56) & 0xFFULL) |
		    ((val >> 40) & 0xFF00ULL) |
		    ((val >> 24) & 0xFF0000ULL) |
		    ((val >> 8)  & 0xFF000000ULL) |
		    ((val << 8)  & 0xFF00000000ULL) |
		    ((val << 24) & 0xFF0000000000ULL) |
		    ((val << 40) & 0xFF000000000000ULL) |
		    ((val << 56) & 0xFF00000000000000ULL);
		break;
	case 4: {	/* CLZ */
		int count = 0;
		int width = sf ? 64 : 32;
		uint64_t mask = 1ULL << (width - 1);
		while (count < width && !(val & mask)) {
			count++;
			mask >>= 1;
		}
		result = (uint64_t)count;
		break;
	}
	case 5: {	/* CLS */
		int count = 0;
		int width = sf ? 64 : 32;
		int sign = (val >> (width - 1)) & 1;
		int i;
		for (i = width - 2; i >= 0; i--) {
			if (((val >> i) & 1) != (unsigned)sign)
				break;
			count++;
		}
		result = (uint64_t)count;
		break;
	}
	default:
		LOG_WARN("unimplemented dp_1src opcode=%u at 0x%llx",
		    opcode, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	if (sf)
		cpu_set_xreg(cpu, rd, result);
	else
		cpu_set_wreg(cpu, rd, (uint32_t)result);

	return EMU_OK;
}

static int
exec_dp_3src(cpu_state_t *cpu, uint32_t insn)
{
	int		sf;
	uint32_t	op31, rm, o0, ra, rn, rd;
	uint64_t	result;

	sf = bit(insn, 31);
	op31 = bits(insn, 23, 21);
	rm = bits(insn, 20, 16);
	o0 = bit(insn, 15);
	ra = bits(insn, 14, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	switch ((op31 << 1) | o0) {
	case 0:	/* MADD */
		if (sf) {
			result = cpu_xreg(cpu, ra) +
			    cpu_xreg(cpu, rn) * cpu_xreg(cpu, rm);
		} else {
			result = (uint64_t)(uint32_t)(cpu_wreg(cpu, ra) +
			    cpu_wreg(cpu, rn) * cpu_wreg(cpu, rm));
		}
		break;
	case 1:	/* MSUB */
		if (sf) {
			result = cpu_xreg(cpu, ra) -
			    cpu_xreg(cpu, rn) * cpu_xreg(cpu, rm);
		} else {
			result = (uint64_t)(uint32_t)(cpu_wreg(cpu, ra) -
			    cpu_wreg(cpu, rn) * cpu_wreg(cpu, rm));
		}
		break;
	case 2:	/* SMADDL (sf must be 1) */
		if (!sf)
			return EMU_ERR;
		result = cpu_xreg(cpu, ra) +
		    (uint64_t)((int64_t)(int32_t)cpu_wreg(cpu, rn) *
		    (int64_t)(int32_t)cpu_wreg(cpu, rm));
		break;
	case 3:	/* SMSUBL */
		if (!sf)
			return EMU_ERR;
		result = cpu_xreg(cpu, ra) -
		    (uint64_t)((int64_t)(int32_t)cpu_wreg(cpu, rn) *
		    (int64_t)(int32_t)cpu_wreg(cpu, rm));
		break;
	case 4: {	/* SMULH */
		int64_t sn, sm;
		uint64_t lo, hi, t1, t2, t3;
		uint64_t an, bn, al, bl, ah, bh;
		int negate;

		if (!sf)
			return EMU_ERR;
		sn = (int64_t)cpu_xreg(cpu, rn);
		sm = (int64_t)cpu_xreg(cpu, rm);
		negate = (sn < 0) != (sm < 0);
		an = (uint64_t)(sn < 0 ? -sn : sn);
		bn = (uint64_t)(sm < 0 ? -sm : sm);
		al = an & 0xFFFFFFFF;
		ah = an >> 32;
		bl = bn & 0xFFFFFFFF;
		bh = bn >> 32;
		lo = al * bl;
		t1 = al * bh + (lo >> 32);
		t2 = ah * bl + (t1 & 0xFFFFFFFF);
		hi = ah * bh + (t1 >> 32) + (t2 >> 32);
		(void)t3;
		if (negate) {
			lo = (al * bl) | ((t2 & 0xFFFFFFFF) << 32);
			if (lo == 0)
				hi = ~hi + 1;
			else
				hi = ~hi;
		}
		result = hi;
		break;
	}
	case 10:	/* UMADDL */
		if (!sf)
			return EMU_ERR;
		result = cpu_xreg(cpu, ra) +
		    (uint64_t)cpu_wreg(cpu, rn) *
		    (uint64_t)cpu_wreg(cpu, rm);
		break;
	case 11:	/* UMSUBL */
		if (!sf)
			return EMU_ERR;
		result = cpu_xreg(cpu, ra) -
		    (uint64_t)cpu_wreg(cpu, rn) *
		    (uint64_t)cpu_wreg(cpu, rm);
		break;
	case 12: {	/* UMULH */
		uint64_t un, um, ul, uh, vl, vh;
		uint64_t t1, t2;

		if (!sf)
			return EMU_ERR;
		un = cpu_xreg(cpu, rn);
		um = cpu_xreg(cpu, rm);
		ul = un & 0xFFFFFFFF;
		uh = un >> 32;
		vl = um & 0xFFFFFFFF;
		vh = um >> 32;
		t1 = ul * vh + ((ul * vl) >> 32);
		t2 = uh * vl + (t1 & 0xFFFFFFFF);
		result = uh * vh + (t1 >> 32) + (t2 >> 32);
		break;
	}
	default:
		LOG_WARN("unimplemented dp_3src op=%u at 0x%llx",
		    (op31 << 1) | o0, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	if (sf)
		cpu_set_xreg(cpu, rd, result);
	else
		cpu_set_wreg(cpu, rd, (uint32_t)result);

	return EMU_OK;
}

int
exec_dp_reg(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	top5;

	/*
	 * Decode using bits[28:24] for the major group.
	 */
	top5 = bits(insn, 28, 24);

	/* Logical shifted register: x0101 0 */
	if ((top5 & 0x1F) == 0x0A)
		return exec_logical_shifted(cpu, insn);

	/* Add/sub shifted/extended register: x0101 1 */
	if ((top5 & 0x1F) == 0x0B) {
		if (bit(insn, 21))
			return exec_add_sub_extended(cpu, insn);
		return exec_add_sub_shifted(cpu, insn);
	}

	/* Conditional compare: bits[28:21] = x1010010 */
	if ((bits(insn, 28, 21) & 0xFE) == 0xD2)
		return exec_cond_compare(cpu, insn);

	/* Conditional select: bits[28:21] = x1010100 */
	if (bits(insn, 28, 21) == 0xD4)
		return exec_cond_select(cpu, insn);

	/* Data processing 2-source: bits[28:21] = x1010110 */
	if (bits(insn, 28, 21) == 0xD6)
		return exec_dp_2src(cpu, insn);

	/* Data processing 1-source: bits[30:21] = 1 x1010110 00 */
	if ((bits(insn, 30, 21) & 0x3FF) == 0x2D6)
		return exec_dp_1src(cpu, insn);

	/* Data processing 3-source: bit[28]=1, bits[24:21] high bit set */
	if (bit(insn, 28) && (bits(insn, 24, 21) & 0x8))
		return exec_dp_3src(cpu, insn);

	LOG_WARN("unimplemented dp_reg at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
