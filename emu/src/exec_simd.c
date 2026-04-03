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

	LOG_WARN("unimplemented SIMD/FP at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
