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

#include <math.h>
#include <string.h>

#include "cpu.h"
#include "decoder.h"
#include "memory.h"
#include "emu.h"
#include "log.h"

/*
 * Scalar floating-point data processing.
 * Encoding: 0001 1110 xx ...
 * type = bits[23:22]: 00 = single, 01 = double
 */

static int
exec_fp_data1(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, opcode, rn, rd;

	type = bits(insn, 23, 22);
	opcode = bits(insn, 20, 15);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	/* FMOV register: opcode = 000000 */
	if (opcode == 0x00) {
		cpu->v[rd] = cpu->v[rn];
		return EMU_OK;
	}

	/* FABS: opcode = 000001 */
	if (opcode == 0x01) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].sf[0] = fabsf(val);
		} else {
			double val = cpu->v[rn].df[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].df[0] = fabs(val);
		}
		return EMU_OK;
	}

	/* FNEG: opcode = 000010 */
	if (opcode == 0x02) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].sf[0] = -val;
		} else {
			double val = cpu->v[rn].df[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].df[0] = -val;
		}
		return EMU_OK;
	}

	/* FSQRT: opcode = 000011 */
	if (opcode == 0x03) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].sf[0] = sqrtf(val);
		} else {
			double val = cpu->v[rn].df[0];
			memset(&cpu->v[rd], 0, sizeof(vreg_t));
			cpu->v[rd].df[0] = sqrt(val);
		}
		return EMU_OK;
	}

	/* FCVT between float types: opcode = 0001xx */
	if ((opcode & 0x3C) == 0x04) {
		uint32_t opc2 = bits(insn, 16, 15);

		memset(&cpu->v[rd], 0, sizeof(vreg_t));

		if (type == 0 && opc2 == 1) {
			/* Single to double */
			cpu->v[rd].df[0] = (double)cpu->v[rn].sf[0];
		} else if (type == 0 && opc2 == 3) {
			/* Single to half - store as float16 */
			/* Simplified: just truncate */
			cpu->v[rd].sf[0] = cpu->v[rn].sf[0];
		} else if (type == 1 && opc2 == 0) {
			/* Double to single */
			cpu->v[rd].sf[0] = (float)cpu->v[rn].df[0];
		} else if (type == 1 && opc2 == 3) {
			/* Double to half */
			cpu->v[rd].sf[0] = (float)cpu->v[rn].df[0];
		} else if (type == 3 && opc2 == 0) {
			/* Half to single */
			cpu->v[rd].sf[0] = cpu->v[rn].sf[0];
		} else if (type == 3 && opc2 == 1) {
			/* Half to double */
			cpu->v[rd].df[0] = (double)cpu->v[rn].sf[0];
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	LOG_WARN("unimplemented fp_data1 opcode=0x%02x at 0x%llx",
	    opcode, (unsigned long long)cpu->pc);
	return EMU_UNIMPL;
}

static int
exec_fp_data2(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, rm, opcode, rn, rd;

	type = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (type == 0) {
		/* Single precision */
		float a = cpu->v[rn].sf[0];
		float b = cpu->v[rm].sf[0];

		switch (opcode) {
		case 0:	/* FMUL */
			cpu->v[rd].sf[0] = a * b;
			break;
		case 1:	/* FDIV */
			cpu->v[rd].sf[0] = a / b;
			break;
		case 2:	/* FADD */
			cpu->v[rd].sf[0] = a + b;
			break;
		case 3:	/* FSUB */
			cpu->v[rd].sf[0] = a - b;
			break;
		case 4:	/* FMAX */
			cpu->v[rd].sf[0] = fmaxf(a, b);
			break;
		case 5:	/* FMIN */
			cpu->v[rd].sf[0] = fminf(a, b);
			break;
		case 8:	/* FNMUL */
			cpu->v[rd].sf[0] = -(a * b);
			break;
		default:
			LOG_WARN("unimplemented fp_data2 opcode=%u at "
			    "0x%llx", opcode,
			    (unsigned long long)cpu->pc);
			return EMU_UNIMPL;
		}
	} else if (type == 1) {
		/* Double precision */
		double a = cpu->v[rn].df[0];
		double b = cpu->v[rm].df[0];

		switch (opcode) {
		case 0:	/* FMUL */
			cpu->v[rd].df[0] = a * b;
			break;
		case 1:	/* FDIV */
			cpu->v[rd].df[0] = a / b;
			break;
		case 2:	/* FADD */
			cpu->v[rd].df[0] = a + b;
			break;
		case 3:	/* FSUB */
			cpu->v[rd].df[0] = a - b;
			break;
		case 4:	/* FMAX */
			cpu->v[rd].df[0] = fmax(a, b);
			break;
		case 5:	/* FMIN */
			cpu->v[rd].df[0] = fmin(a, b);
			break;
		case 8:	/* FNMUL */
			cpu->v[rd].df[0] = -(a * b);
			break;
		default:
			LOG_WARN("unimplemented fp_data2 opcode=%u at "
			    "0x%llx", opcode,
			    (unsigned long long)cpu->pc);
			return EMU_UNIMPL;
		}
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

static int
exec_fp_compare(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, rm, rn, opcode2;
	int		cmp_zero;

	type = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	opcode2 = bits(insn, 4, 0);

	cmp_zero = opcode2 & 0x08;

	if (type == 0) {
		/* Single precision */
		float a = cpu->v[rn].sf[0];
		float b = cmp_zero ? 0.0f : cpu->v[rm].sf[0];

		if (isnan(a) || isnan(b)) {
			cpu->nzcv = 0x3 << 28;		/* 0011: unordered */
		} else if (a == b) {
			cpu->nzcv = 0x6 << 28;		/* 0110: equal */
		} else if (a < b) {
			cpu->nzcv = 0x8u << 28;	/* 1000: less than */
		} else {
			cpu->nzcv = 0x2 << 28;		/* 0010: greater */
		}
	} else if (type == 1) {
		/* Double precision */
		double a = cpu->v[rn].df[0];
		double b = cmp_zero ? 0.0 : cpu->v[rm].df[0];

		if (isnan(a) || isnan(b)) {
			cpu->nzcv = 0x3 << 28;
		} else if (a == b) {
			cpu->nzcv = 0x6 << 28;
		} else if (a < b) {
			cpu->nzcv = 0x8u << 28;
		} else {
			cpu->nzcv = 0x2 << 28;
		}
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

static int
exec_fp_csel(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, rm, cond, rn, rd;

	type = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	cond = bits(insn, 15, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (cpu_check_cond(cpu, cond)) {
		if (type == 0)
			cpu->v[rd].sf[0] = cpu->v[rn].sf[0];
		else
			cpu->v[rd].df[0] = cpu->v[rn].df[0];
	} else {
		if (type == 0)
			cpu->v[rd].sf[0] = cpu->v[rm].sf[0];
		else
			cpu->v[rd].df[0] = cpu->v[rm].df[0];
	}

	return EMU_OK;
}

static int
exec_fp_int_conv(cpu_state_t *cpu, uint32_t insn)
{
	int		sf;
	uint32_t	type, rmode, opcode, rn, rd;

	sf = bit(insn, 31);
	type = bits(insn, 23, 22);
	rmode = bits(insn, 20, 19);
	opcode = bits(insn, 18, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	/* FMOV: GPR to FP or FP to GPR */
	if (rmode == 0 && opcode == 7) {
		/* FMOV GPR -> FP: Wn/Xn to Vd */
		memset(&cpu->v[rd], 0, sizeof(vreg_t));
		if (type == 0) {
			/* 32-bit GPR to single */
			cpu->v[rd].s[0] = cpu_wreg(cpu, rn);
		} else if (type == 1 && sf) {
			/* 64-bit GPR to double */
			cpu->v[rd].d[0] = cpu_xreg(cpu, rn);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	if (rmode == 0 && opcode == 6) {
		/* FMOV FP -> GPR: Vn to Wd/Xd */
		if (type == 0) {
			cpu_set_wreg(cpu, rd, cpu->v[rn].s[0]);
		} else if (type == 1 && sf) {
			cpu_set_xreg(cpu, rd, cpu->v[rn].d[0]);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* FMOV top half: GPR to V.d[1] or V.d[1] to GPR */
	if (rmode == 1 && opcode == 7 && type == 2) {
		/* GPR -> V.d[1] */
		cpu->v[rd].d[1] = cpu_xreg(cpu, rn);
		return EMU_OK;
	}
	if (rmode == 1 && opcode == 6 && type == 2) {
		/* V.d[1] -> GPR */
		cpu_set_xreg(cpu, rd, cpu->v[rn].d[1]);
		return EMU_OK;
	}

	/* SCVTF: signed int to float */
	if (rmode == 0 && opcode == 2) {
		memset(&cpu->v[rd], 0, sizeof(vreg_t));
		if (type == 0) {
			if (sf)
				cpu->v[rd].sf[0] =
				    (float)(int64_t)cpu_xreg(cpu, rn);
			else
				cpu->v[rd].sf[0] =
				    (float)(int32_t)cpu_wreg(cpu, rn);
		} else if (type == 1) {
			if (sf)
				cpu->v[rd].df[0] =
				    (double)(int64_t)cpu_xreg(cpu, rn);
			else
				cpu->v[rd].df[0] =
				    (double)(int32_t)cpu_wreg(cpu, rn);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* UCVTF: unsigned int to float */
	if (rmode == 0 && opcode == 3) {
		memset(&cpu->v[rd], 0, sizeof(vreg_t));
		if (type == 0) {
			if (sf)
				cpu->v[rd].sf[0] =
				    (float)cpu_xreg(cpu, rn);
			else
				cpu->v[rd].sf[0] =
				    (float)cpu_wreg(cpu, rn);
		} else if (type == 1) {
			if (sf)
				cpu->v[rd].df[0] =
				    (double)cpu_xreg(cpu, rn);
			else
				cpu->v[rd].df[0] =
				    (double)cpu_wreg(cpu, rn);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* FCVTZS: float to signed int, truncate toward zero */
	if (rmode == 3 && opcode == 0) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			if (sf)
				cpu_set_xreg(cpu, rd,
				    (uint64_t)(int64_t)val);
			else
				cpu_set_wreg(cpu, rd,
				    (uint32_t)(int32_t)val);
		} else if (type == 1) {
			double val = cpu->v[rn].df[0];
			if (sf)
				cpu_set_xreg(cpu, rd,
				    (uint64_t)(int64_t)val);
			else
				cpu_set_wreg(cpu, rd,
				    (uint32_t)(int32_t)val);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* FCVTZU: float to unsigned int, truncate toward zero */
	if (rmode == 3 && opcode == 1) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			if (val < 0.0f)
				val = 0.0f;
			if (sf)
				cpu_set_xreg(cpu, rd, (uint64_t)val);
			else
				cpu_set_wreg(cpu, rd, (uint32_t)val);
		} else if (type == 1) {
			double val = cpu->v[rn].df[0];
			if (val < 0.0)
				val = 0.0;
			if (sf)
				cpu_set_xreg(cpu, rd, (uint64_t)val);
			else
				cpu_set_wreg(cpu, rd, (uint32_t)val);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* FCVTNS/FCVTNU/FCVTPS/FCVTMS etc. - rounding variants */
	if (opcode == 0 || opcode == 1) {
		/* Various rounding modes to integer */
		double val;
		int is_signed = (opcode == 0);

		if (type == 0)
			val = (double)cpu->v[rn].sf[0];
		else if (type == 1)
			val = cpu->v[rn].df[0];
		else
			return EMU_UNIMPL;

		switch (rmode) {
		case 0:	/* Nearest, ties to even */
			val = rint(val);
			break;
		case 1:	/* Toward +infinity */
			val = ceil(val);
			break;
		case 2:	/* Toward -infinity */
			val = floor(val);
			break;
		case 3:	/* Toward zero */
			val = trunc(val);
			break;
		}

		if (is_signed) {
			if (sf)
				cpu_set_xreg(cpu, rd,
				    (uint64_t)(int64_t)val);
			else
				cpu_set_wreg(cpu, rd,
				    (uint32_t)(int32_t)val);
		} else {
			if (val < 0.0)
				val = 0.0;
			if (sf)
				cpu_set_xreg(cpu, rd, (uint64_t)val);
			else
				cpu_set_wreg(cpu, rd, (uint32_t)val);
		}
		return EMU_OK;
	}

	LOG_WARN("unimplemented fp_int_conv rmode=%u opcode=%u at 0x%llx",
	    rmode, opcode, (unsigned long long)cpu->pc);
	return EMU_UNIMPL;
}

static int
exec_fp_data3(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, o1, rm, o0, ra, rn, rd;

	type = bits(insn, 23, 22);
	o1 = bit(insn, 21);
	rm = bits(insn, 20, 16);
	o0 = bit(insn, 15);
	ra = bits(insn, 14, 10);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (type == 0) {
		float a = cpu->v[rn].sf[0];
		float b = cpu->v[rm].sf[0];
		float c = cpu->v[ra].sf[0];

		switch ((o1 << 1) | o0) {
		case 0:	/* FMADD */
			cpu->v[rd].sf[0] = c + a * b;
			break;
		case 1:	/* FMSUB */
			cpu->v[rd].sf[0] = c - a * b;
			break;
		case 2:	/* FNMADD */
			cpu->v[rd].sf[0] = -(c + a * b);
			break;
		case 3:	/* FNMSUB */
			cpu->v[rd].sf[0] = -(c - a * b);
			break;
		}
	} else if (type == 1) {
		double a = cpu->v[rn].df[0];
		double b = cpu->v[rm].df[0];
		double c = cpu->v[ra].df[0];

		switch ((o1 << 1) | o0) {
		case 0:	/* FMADD */
			cpu->v[rd].df[0] = c + a * b;
			break;
		case 1:	/* FMSUB */
			cpu->v[rd].df[0] = c - a * b;
			break;
		case 2:	/* FNMADD */
			cpu->v[rd].df[0] = -(c + a * b);
			break;
		case 3:	/* FNMSUB */
			cpu->v[rd].df[0] = -(c - a * b);
			break;
		}
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

/*
 * DUP (general): replicate GPR value to all SIMD vector elements.
 * Encoding: 0 Q 0 01110000 imm5 0 00011 Rn Rd
 * imm5 determines element size: bit[0]=1 -> B, bit[1]=1 -> H,
 * bit[2]=1 -> S, bit[3]=1 -> D
 */
static int
exec_dup_general(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, imm5, rn, rd;
	uint64_t	val;
	int		esize, i, elems;

	Q = bit(insn, 30);
	imm5 = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	val = cpu_xreg(cpu, rn);
	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (imm5 & 0x01) {
		/* Byte */
		esize = 1;
		elems = Q ? 16 : 8;
		for (i = 0; i < elems; i++)
			cpu->v[rd].b[i] = (uint8_t)val;
	} else if (imm5 & 0x02) {
		/* Halfword */
		esize = 2;
		elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++)
			cpu->v[rd].h[i] = (uint16_t)val;
	} else if (imm5 & 0x04) {
		/* Word */
		esize = 4;
		elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = (uint32_t)val;
	} else if (imm5 & 0x08) {
		/* Doubleword */
		esize = 8;
		elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++)
			cpu->v[rd].d[i] = val;
	} else {
		return EMU_UNIMPL;
	}

	(void)esize;
	return EMU_OK;
}

/*
 * CMEQ (vector, zero): compare each element with 0.
 * If equal, set all bits of the result element; otherwise clear.
 * Encoding: 0 Q 1 01110 size 10000 0100 1 10 Rn Rd
 */
static int
exec_cmeq_zero(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, size, rn, rd;
	int		i, elems;

	Q = bit(insn, 30);
	size = bits(insn, 23, 22);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	switch (size) {
	case 0:	/* 8-bit */
		elems = Q ? 16 : 8;
		for (i = 0; i < elems; i++)
			cpu->v[rd].b[i] =
			    cpu->v[rn].b[i] == 0 ? 0xFF : 0x00;
		break;
	case 1:	/* 16-bit */
		elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++)
			cpu->v[rd].h[i] =
			    cpu->v[rn].h[i] == 0 ? 0xFFFF : 0x0000;
		break;
	case 2:	/* 32-bit */
		elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] =
			    cpu->v[rn].s[i] == 0 ? 0xFFFFFFFF : 0;
		break;
	case 3:	/* 64-bit */
		elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++)
			cpu->v[rd].d[i] =
			    cpu->v[rn].d[i] == 0 ? ~0ULL : 0;
		break;
	}

	return EMU_OK;
}

/*
 * UMAXV: unsigned maximum across vector.
 * Encoding: 0 Q 1 01110 size 11000 1 1010 10 Rn Rd
 * Reduces vector to a single scalar (maximum of all elements).
 */
static int
exec_umaxv(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, size, rn, rd;
	int		i, elems;

	Q = bit(insn, 30);
	size = bits(insn, 23, 22);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	switch (size) {
	case 0: {	/* 8-bit */
		uint8_t max = 0;
		elems = Q ? 16 : 8;
		for (i = 0; i < elems; i++) {
			if (cpu->v[rn].b[i] > max)
				max = cpu->v[rn].b[i];
		}
		cpu->v[rd].b[0] = max;
		break;
	}
	case 1: {	/* 16-bit */
		uint16_t max = 0;
		elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++) {
			if (cpu->v[rn].h[i] > max)
				max = cpu->v[rn].h[i];
		}
		cpu->v[rd].h[0] = max;
		break;
	}
	case 2: {	/* 32-bit */
		uint32_t max = 0;
		elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++) {
			if (cpu->v[rn].s[i] > max)
				max = cpu->v[rn].s[i];
		}
		cpu->v[rd].s[0] = max;
		break;
	}
	default:
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

/*
 * ORR (vector): bitwise OR of two SIMD registers.
 * Encoding: 0 Q 0 01110 10 1 Rm 0 00111 Rn Rd
 */
static int
exec_orr_vector(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, rm, rn, rd;
	int		bytes, i;

	Q = bit(insn, 30);
	rm = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	bytes = Q ? 16 : 8;
	memset(&cpu->v[rd], 0, sizeof(vreg_t));
	for (i = 0; i < bytes; i++)
		cpu->v[rd].b[i] = cpu->v[rn].b[i] | cpu->v[rm].b[i];

	return EMU_OK;
}

/*
 * AND (vector): bitwise AND of two SIMD registers.
 * Encoding: 0 Q 0 01110 00 1 Rm 0 00111 Rn Rd
 */
static int
exec_and_vector(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, rm, rn, rd;
	int		bytes, i;

	Q = bit(insn, 30);
	rm = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	bytes = Q ? 16 : 8;
	memset(&cpu->v[rd], 0, sizeof(vreg_t));
	for (i = 0; i < bytes; i++)
		cpu->v[rd].b[i] = cpu->v[rn].b[i] & cpu->v[rm].b[i];

	return EMU_OK;
}

/*
 * MOVI: move immediate to vector.
 * Encoding: 0 Q op 0111100000 a:b:c cmode 01 d:e:f:g:h Rd
 * Simplified: handles the common 8-bit broadcast (cmode=1110, op=0).
 */
static int
exec_movi(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, op, cmode, rd;
	uint8_t		imm8;
	int		bytes, i;

	Q = bit(insn, 30);
	op = bit(insn, 29);
	cmode = bits(insn, 15, 12);
	rd = bits(insn, 4, 0);

	/* Reconstruct 8-bit immediate: a:b:c:d:e:f:g:h */
	imm8 = (uint8_t)((bits(insn, 18, 16) << 5) | bits(insn, 9, 5));

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (op == 0 && cmode == 0x0E) {
		/* MOVI Vd.{8B|16B}, #imm8 */
		bytes = Q ? 16 : 8;
		for (i = 0; i < bytes; i++)
			cpu->v[rd].b[i] = imm8;
		return EMU_OK;
	}

	if (op == 0 && (cmode & 0x0E) == 0x00) {
		/* MOVI Vd.{2S|4S}, #imm8{, LSL #0} */
		uint32_t shift = (cmode & 1) ? 8 : 0;
		uint32_t val = (uint32_t)imm8 << shift;
		int elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = val;
		return EMU_OK;
	}

	if (op == 0 && (cmode & 0x0E) == 0x02) {
		/* MOVI Vd.{2S|4S}, #imm8, LSL #8 or #24 */
		uint32_t shift = (cmode & 1) ? 24 : 16;
		uint32_t val = (uint32_t)imm8 << shift;
		int elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = val;
		return EMU_OK;
	}

	if (op == 0 && (cmode & 0x0E) == 0x04) {
		/* MOVI Vd.{2S|4S}, #imm8, LSL #16 */
		uint32_t shift = (cmode & 1) ? 8 : 0;
		uint32_t val = (uint32_t)imm8 << shift;
		int elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++)
			cpu->v[rd].h[i] = (uint16_t)val;
		return EMU_OK;
	}

	if (op == 0 && (cmode & 0x0E) == 0x0C) {
		/* MOVI Vd.{2S|4S}, #imm8, MSL #8 or MSL #16 */
		uint32_t shift = (cmode & 1) ? 16 : 8;
		uint32_t val = ((uint32_t)imm8 << shift) |
		    ((1u << shift) - 1);
		int elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = val;
		return EMU_OK;
	}

	if (op == 1 && cmode == 0x0E) {
		/* MOVI Dd/MOVI Vd.2D, #imm64 */
		/* Each bit of imm8 maps to a byte of 0x00 or 0xFF */
		uint64_t val = 0;
		for (i = 0; i < 8; i++) {
			if (imm8 & (1u << i))
				val |= 0xFFULL << (i * 8);
		}
		cpu->v[rd].d[0] = val;
		if (Q)
			cpu->v[rd].d[1] = val;
		return EMU_OK;
	}

	LOG_WARN("unimplemented MOVI op=%u cmode=0x%x at 0x%llx",
	    op, cmode, (unsigned long long)cpu->pc);
	return EMU_UNIMPL;
}

/*
 * AdvSIMD load/store single structure (1 register).
 * LD1 no-offset: 0 Q 00 1100 01 0 00000 0111 size Rn Rt
 * ST1 no-offset: 0 Q 00 1100 00 0 00000 0111 size Rn Rt
 * LD1 post-index: 0 Q 00 1100 11 0 11111 0111 size Rn Rt  (imm)
 * ST1 post-index: 0 Q 00 1100 10 0 11111 0111 size Rn Rt  (imm)
 * LD1 post-index: 0 Q 00 1100 11 0 Rm    0111 size Rn Rt  (reg)
 * ST1 post-index: 0 Q 00 1100 10 0 Rm    0111 size Rn Rt  (reg)
 */
static int
exec_simd_ldst1(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, L, rm, opcode, size, rn, rt;
	uint64_t	addr;
	int		bytes, i;
	int		post;
	void		*host;

	Q = bit(insn, 30);
	L = bit(insn, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 12);
	size = bits(insn, 11, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);
	post = bit(insn, 23);

	/* opcode 0111 = single register, 1 structure */
	if (opcode != 0x07) {
		LOG_WARN("unimplemented SIMD ldst opcode=0x%x at 0x%llx",
		    opcode, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	(void)size;
	bytes = Q ? 16 : 8;
	addr = cpu_xreg_sp(cpu, rn);

	if (L) {
		/* LD1 */
		memset(&cpu->v[rt], 0, sizeof(vreg_t));
		host = mem_translate(cpu->mem, addr, (uint64_t)bytes,
		    MEM_PROT_READ);
		if (host != NULL) {
			memcpy(&cpu->v[rt], host, (size_t)bytes);
		} else {
			for (i = 0; i < bytes; i++) {
				if (mem_read8(cpu->mem, addr + (uint64_t)i,
				    &cpu->v[rt].b[i]) != 0)
					return EMU_SEGFAULT;
			}
		}
	} else {
		/* ST1 */
		host = mem_translate(cpu->mem, addr, (uint64_t)bytes,
		    MEM_PROT_WRITE);
		if (host != NULL) {
			memcpy(host, &cpu->v[rt], (size_t)bytes);
		} else {
			for (i = 0; i < bytes; i++) {
				if (mem_write8(cpu->mem, addr + (uint64_t)i,
				    cpu->v[rt].b[i]) != 0)
					return EMU_SEGFAULT;
			}
		}
	}

	/* Post-index writeback */
	if (post) {
		uint64_t offset;
		if (rm == 31) {
			/* Immediate: advance by vector length */
			offset = (uint64_t)bytes;
		} else {
			/* Register: advance by Xm */
			offset = cpu_xreg(cpu, rm);
		}
		cpu_set_xreg_sp(cpu, rn, addr + offset);
	}

	return EMU_OK;
}

int
exec_simd(cpu_state_t *cpu, uint32_t insn)
{
	/*
	 * Scalar FP: bits[28:24] = 11110
	 * Check for scalar FP instructions first.
	 */
	if (bits(insn, 28, 24) == 0x1E) {
		/* FP data processing 1-source: bits[14:10] = 10000 */
		if (bits(insn, 14, 10) == 0x10 && bit(insn, 21) == 1 &&
		    bits(insn, 11, 10) == 0)
			return exec_fp_data1(cpu, insn);

		/* FP compare: bits[13:10] = 1000, bits[4:2] = 000 */
		if (bits(insn, 13, 10) == 0x08 && bits(insn, 4, 2) == 0)
			return exec_fp_compare(cpu, insn);

		/* FP data processing 2-source: bit[11] = 1, bit[10] = 0 */
		if (bit(insn, 11) == 1 && bit(insn, 10) == 0 &&
		    bit(insn, 21) == 1 && bits(insn, 14, 12) != 4)
			return exec_fp_data2(cpu, insn);

		/* FP conditional select: bits[11:10] = 11 */
		if (bits(insn, 11, 10) == 3 && bit(insn, 21) == 1)
			return exec_fp_csel(cpu, insn);

		/* FP <-> integer conversion: bit[21] = 1,
		 * bits[14:10] = 00000 */
		if (bit(insn, 21) == 1 && bits(insn, 14, 10) == 0x00)
			return exec_fp_int_conv(cpu, insn);
	}

	/* FP data processing 3-source: bits[28:24] = 11111 */
	if (bits(insn, 28, 24) == 0x1F)
		return exec_fp_data3(cpu, insn);

	/*
	 * Advanced SIMD data processing.
	 * These share bits[28:24] = 0x0E or 0x0F.
	 */

	/* DUP (general): 0 Q 0 01110000 imm5 0 00011 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x03)
		return exec_dup_general(cpu, insn);

	/* UMOV / MOV (to general): 0 Q 0 01110000 imm5 0 01111 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x0F) {
		uint32_t imm5 = bits(insn, 20, 16);
		uint32_t rn = bits(insn, 9, 5);
		uint32_t rd = bits(insn, 4, 0);
		uint64_t val = 0;

		if (imm5 & 1) {		/* byte */
			int idx = (imm5 >> 1) & 0xF;
			val = cpu->v[rn].b[idx];
		} else if (imm5 & 2) {	/* halfword */
			int idx = (imm5 >> 2) & 0x7;
			val = cpu->v[rn].h[idx];
		} else if (imm5 & 4) {	/* word */
			int idx = (imm5 >> 3) & 0x3;
			val = cpu->v[rn].s[idx];
		} else if (imm5 & 8) {	/* doubleword */
			int idx = (imm5 >> 4) & 0x1;
			val = cpu->v[rn].d[idx];
		}
		cpu_set_xreg(cpu, rd, val);
		return EMU_OK;
	}

	/*
	 * CMEQ (zero): 0 Q 1 01110 size 10000 0100 1 10 Rn Rd
	 * bits[29] = 1, bits[28:24] = 01110, bits[20:17] = 1000,
	 * bit[16] = 0, bits[15:10] = 100110 (opcode=01001, bit10=0 -> 10)
	 *
	 * More precisely:
	 * bits[31] = 0, [30] = Q, [29] = 1, [28:24] = 01110,
	 * [23:22] = size, [21:17] = 10000, [16] = 0,
	 * [15:10] = 100110 (= 0x26)
	 */
	if (bit(insn, 29) == 1 && bits(insn, 28, 24) == 0x0E &&
	    bits(insn, 21, 17) == 0x10 && bit(insn, 16) == 0 &&
	    bits(insn, 15, 10) == 0x26)
		return exec_cmeq_zero(cpu, insn);

	/*
	 * UMAXV: 0 Q 1 01110 size 11000 1 1010 10 Rn Rd
	 * bits[29] = 1, bits[28:24] = 01110,
	 * bits[21:17] = 11000, bit[16] = 1,
	 * bits[15:10] = 101010 (= 0x2A)
	 */
	if (bit(insn, 29) == 1 && bits(insn, 28, 24) == 0x0E &&
	    bits(insn, 21, 17) == 0x18 && bit(insn, 16) == 1 &&
	    bits(insn, 15, 10) == 0x2A)
		return exec_umaxv(cpu, insn);

	/*
	 * ORR (vector): 0 Q 0 01110 10 1 Rm 0 00111 Rn Rd
	 * bits[29] = 0, bits[28:24] = 01110, bits[23:21] = 101,
	 * bits[15:10] = 000111 (= 0x07)
	 */
	if (bit(insn, 29) == 0 && bits(insn, 28, 24) == 0x0E &&
	    bits(insn, 23, 21) == 0x05 && bits(insn, 15, 10) == 0x07)
		return exec_orr_vector(cpu, insn);

	/*
	 * AND (vector): 0 Q 0 01110 00 1 Rm 0 00111 Rn Rd
	 * bits[29] = 0, bits[28:24] = 01110, bits[23:21] = 001,
	 * bits[15:10] = 000111 (= 0x07)
	 */
	if (bit(insn, 29) == 0 && bits(insn, 28, 24) == 0x0E &&
	    bits(insn, 23, 21) == 0x01 && bits(insn, 15, 10) == 0x07)
		return exec_and_vector(cpu, insn);

	/*
	 * MOVI: 0 Q op 0111100000 abc cmode 01 defgh Rd
	 * bits[28:19] = 0111100000
	 * bits[11:10] = 01
	 */
	if (bits(insn, 28, 19) == 0x1E0 && bits(insn, 11, 10) == 0x01)
		return exec_movi(cpu, insn);

	/*
	 * AdvSIMD LD1/ST1 single structure.
	 * bits[31] = 0, bits[29:24] = 001100
	 */
	if (bit(insn, 31) == 0 && bits(insn, 29, 24) == 0x0C &&
	    bit(insn, 21) == 0)
		return exec_simd_ldst1(cpu, insn);

	LOG_WARN("unimplemented SIMD/FP at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
