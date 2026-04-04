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

/* ---- Saturation helpers ---- */

static inline int64_t
sat_signed(int64_t val, int bits_w)
{
	int64_t max = ((int64_t)1 << (bits_w - 1)) - 1;
	int64_t min = -((int64_t)1 << (bits_w - 1));

	if (val > max)
		return max;
	if (val < min)
		return min;
	return val;
}

static inline uint64_t
sat_unsigned(int64_t val, int bits_w)
{
	uint64_t max = ((uint64_t)1 << bits_w) - 1;

	if (val < 0)
		return 0;
	if ((uint64_t)val > max)
		return max;
	return (uint64_t)val;
}

static inline uint64_t
usat_unsigned(uint64_t val, int bits_w)
{
	uint64_t max = (bits_w >= 64) ? ~0ULL : ((uint64_t)1 << bits_w) - 1;

	if (val > max)
		return max;
	return val;
}

/* Read element as signed, given size code (0=8b,1=16b,2=32b,3=64b) */
static inline int64_t
velem_s(const vreg_t *v, int size, int idx)
{
	switch (size) {
	case 0: return (int8_t)v->b[idx];
	case 1: return (int16_t)v->h[idx];
	case 2: return (int32_t)v->s[idx];
	case 3: return (int64_t)v->d[idx];
	}
	return 0;
}

/* Read element as unsigned */
static inline uint64_t
velem_u(const vreg_t *v, int size, int idx)
{
	switch (size) {
	case 0: return v->b[idx];
	case 1: return v->h[idx];
	case 2: return v->s[idx];
	case 3: return v->d[idx];
	}
	return 0;
}

/* Write element */
static inline void
velem_set(vreg_t *v, int size, int idx, uint64_t val)
{
	switch (size) {
	case 0: v->b[idx] = (uint8_t)val; break;
	case 1: v->h[idx] = (uint16_t)val; break;
	case 2: v->s[idx] = (uint32_t)val; break;
	case 3: v->d[idx] = val; break;
	}
}

/* Number of elements for given size and Q */
static inline int
velem_count(int size, int Q)
{
	return (Q ? 16 : 8) >> size;
}

/* Bit width for element size */
static inline int
esize_bits(int size)
{
	return 8 << size;
}

/* All-ones mask for element */
static inline uint64_t
esize_mask(int size)
{
	if (size == 3)
		return ~0ULL;
	return ((uint64_t)1 << esize_bits(size)) - 1;
}

/* Reverse bits in a byte */
static inline uint8_t
rbit8(uint8_t v)
{
	v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
	v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
	v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
	return v;
}

/* Count leading zeros for n-bit value */
static inline int
clz_n(uint64_t val, int nbits)
{
	int cnt = 0;

	for (int i = nbits - 1; i >= 0; i--) {
		if (val & ((uint64_t)1 << i))
			break;
		cnt++;
	}
	return cnt;
}

/* Count leading sign bits for n-bit value */
static inline int
cls_n(uint64_t val, int nbits)
{
	int sign = (val >> (nbits - 1)) & 1;
	int cnt = 0;

	for (int i = nbits - 2; i >= 0; i--) {
		if (((val >> i) & 1) != (uint64_t)sign)
			break;
		cnt++;
	}
	return cnt;
}

/* ---- Scalar FP data processing ---- */

static int
exec_fp_data1(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	type, opcode, rn, rd;

	type = bits(insn, 23, 22);
	opcode = bits(insn, 20, 15);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (opcode == 0x00) {
		/* FMOV register */
		cpu->v[rd] = cpu->v[rn];
		return EMU_OK;
	}

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (opcode == 0x01) {
		/* FABS */
		if (type == 0)
			cpu->v[rd].sf[0] = fabsf(cpu->v[rn].sf[0]);
		else
			cpu->v[rd].df[0] = fabs(cpu->v[rn].df[0]);
		return EMU_OK;
	}

	if (opcode == 0x02) {
		/* FNEG */
		if (type == 0)
			cpu->v[rd].sf[0] = -cpu->v[rn].sf[0];
		else
			cpu->v[rd].df[0] = -cpu->v[rn].df[0];
		return EMU_OK;
	}

	if (opcode == 0x03) {
		/* FSQRT */
		if (type == 0)
			cpu->v[rd].sf[0] = sqrtf(cpu->v[rn].sf[0]);
		else
			cpu->v[rd].df[0] = sqrt(cpu->v[rn].df[0]);
		return EMU_OK;
	}

	/* FRINTN/FRINTP/FRINTM/FRINTZ/FRINTA/FRINTX/FRINTI */
	if (opcode >= 0x08 && opcode <= 0x0F) {
		double val;

		if (type == 0)
			val = (double)cpu->v[rn].sf[0];
		else if (type == 1)
			val = cpu->v[rn].df[0];
		else
			return EMU_UNIMPL;

		switch (opcode) {
		case 0x08: val = rint(val); break;	/* FRINTN */
		case 0x09: val = ceil(val); break;	/* FRINTP */
		case 0x0A: val = floor(val); break;	/* FRINTM */
		case 0x0B: val = trunc(val); break;	/* FRINTZ */
		case 0x0C:				/* FRINTA */
			val = round(val);
			break;
		case 0x0E: val = rint(val); break;	/* FRINTX */
		case 0x0F: val = rint(val); break;	/* FRINTI */
		default: return EMU_UNIMPL;
		}

		if (type == 0)
			cpu->v[rd].sf[0] = (float)val;
		else
			cpu->v[rd].df[0] = val;
		return EMU_OK;
	}

	/* FCVT between float types */
	if ((opcode & 0x3C) == 0x04) {
		uint32_t opc2 = bits(insn, 16, 15);

		if (type == 0 && opc2 == 1)
			cpu->v[rd].df[0] = (double)cpu->v[rn].sf[0];
		else if (type == 0 && opc2 == 3)
			cpu->v[rd].sf[0] = cpu->v[rn].sf[0];
		else if (type == 1 && opc2 == 0)
			cpu->v[rd].sf[0] = (float)cpu->v[rn].df[0];
		else if (type == 1 && opc2 == 3)
			cpu->v[rd].sf[0] = (float)cpu->v[rn].df[0];
		else if (type == 3 && opc2 == 0)
			cpu->v[rd].sf[0] = cpu->v[rn].sf[0];
		else if (type == 3 && opc2 == 1)
			cpu->v[rd].df[0] = (double)cpu->v[rn].sf[0];
		else
			return EMU_UNIMPL;
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
		float a = cpu->v[rn].sf[0];
		float b = cpu->v[rm].sf[0];

		switch (opcode) {
		case 0: cpu->v[rd].sf[0] = a * b; break;
		case 1: cpu->v[rd].sf[0] = a / b; break;
		case 2: cpu->v[rd].sf[0] = a + b; break;
		case 3: cpu->v[rd].sf[0] = a - b; break;
		case 4: cpu->v[rd].sf[0] = fmaxf(a, b); break;
		case 5: cpu->v[rd].sf[0] = fminf(a, b); break;
		case 6:	/* FMAXNM */
			cpu->v[rd].sf[0] = (isnan(a)) ? b :
			    (isnan(b)) ? a : fmaxf(a, b);
			break;
		case 7:	/* FMINNM */
			cpu->v[rd].sf[0] = (isnan(a)) ? b :
			    (isnan(b)) ? a : fminf(a, b);
			break;
		case 8: cpu->v[rd].sf[0] = -(a * b); break;
		default:
			LOG_WARN("unimplemented fp_data2 opcode=%u", opcode);
			return EMU_UNIMPL;
		}
	} else if (type == 1) {
		double a = cpu->v[rn].df[0];
		double b = cpu->v[rm].df[0];

		switch (opcode) {
		case 0: cpu->v[rd].df[0] = a * b; break;
		case 1: cpu->v[rd].df[0] = a / b; break;
		case 2: cpu->v[rd].df[0] = a + b; break;
		case 3: cpu->v[rd].df[0] = a - b; break;
		case 4: cpu->v[rd].df[0] = fmax(a, b); break;
		case 5: cpu->v[rd].df[0] = fmin(a, b); break;
		case 6:	/* FMAXNM */
			cpu->v[rd].df[0] = (isnan(a)) ? b :
			    (isnan(b)) ? a : fmax(a, b);
			break;
		case 7:	/* FMINNM */
			cpu->v[rd].df[0] = (isnan(a)) ? b :
			    (isnan(b)) ? a : fmin(a, b);
			break;
		case 8: cpu->v[rd].df[0] = -(a * b); break;
		default:
			LOG_WARN("unimplemented fp_data2 opcode=%u", opcode);
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
		float a = cpu->v[rn].sf[0];
		float b = cmp_zero ? 0.0f : cpu->v[rm].sf[0];

		if (isnan(a) || isnan(b))
			cpu->nzcv = 0x3 << 28;
		else if (a == b)
			cpu->nzcv = 0x6 << 28;
		else if (a < b)
			cpu->nzcv = 0x8u << 28;
		else
			cpu->nzcv = 0x2 << 28;
	} else if (type == 1) {
		double a = cpu->v[rn].df[0];
		double b = cmp_zero ? 0.0 : cpu->v[rm].df[0];

		if (isnan(a) || isnan(b))
			cpu->nzcv = 0x3 << 28;
		else if (a == b)
			cpu->nzcv = 0x6 << 28;
		else if (a < b)
			cpu->nzcv = 0x8u << 28;
		else
			cpu->nzcv = 0x2 << 28;
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
		memset(&cpu->v[rd], 0, sizeof(vreg_t));
		if (type == 0)
			cpu->v[rd].s[0] = cpu_wreg(cpu, rn);
		else if (type == 1 && sf)
			cpu->v[rd].d[0] = cpu_xreg(cpu, rn);
		else
			return EMU_UNIMPL;
		return EMU_OK;
	}

	if (rmode == 0 && opcode == 6) {
		if (type == 0)
			cpu_set_wreg(cpu, rd, cpu->v[rn].s[0]);
		else if (type == 1 && sf)
			cpu_set_xreg(cpu, rd, cpu->v[rn].d[0]);
		else
			return EMU_UNIMPL;
		return EMU_OK;
	}

	/* FMOV top half */
	if (rmode == 1 && opcode == 7 && type == 2) {
		cpu->v[rd].d[1] = cpu_xreg(cpu, rn);
		return EMU_OK;
	}
	if (rmode == 1 && opcode == 6 && type == 2) {
		cpu_set_xreg(cpu, rd, cpu->v[rn].d[1]);
		return EMU_OK;
	}

	/* SCVTF */
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

	/* UCVTF */
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

	/* FCVTZS */
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

	/* FCVTZU */
	if (rmode == 3 && opcode == 1) {
		if (type == 0) {
			float val = cpu->v[rn].sf[0];
			if (val < 0.0f) val = 0.0f;
			if (sf)
				cpu_set_xreg(cpu, rd, (uint64_t)val);
			else
				cpu_set_wreg(cpu, rd, (uint32_t)val);
		} else if (type == 1) {
			double val = cpu->v[rn].df[0];
			if (val < 0.0) val = 0.0;
			if (sf)
				cpu_set_xreg(cpu, rd, (uint64_t)val);
			else
				cpu_set_wreg(cpu, rd, (uint32_t)val);
		} else {
			return EMU_UNIMPL;
		}
		return EMU_OK;
	}

	/* FCVTNS/NU/PS/PU/MS/MU/AS/AU rounding variants */
	if (opcode == 0 || opcode == 1) {
		double val;
		int is_signed = (opcode == 0);

		if (type == 0)
			val = (double)cpu->v[rn].sf[0];
		else if (type == 1)
			val = cpu->v[rn].df[0];
		else
			return EMU_UNIMPL;

		switch (rmode) {
		case 0: val = rint(val); break;
		case 1: val = ceil(val); break;
		case 2: val = floor(val); break;
		case 3: val = trunc(val); break;
		}

		if (is_signed) {
			if (sf)
				cpu_set_xreg(cpu, rd,
				    (uint64_t)(int64_t)val);
			else
				cpu_set_wreg(cpu, rd,
				    (uint32_t)(int32_t)val);
		} else {
			if (val < 0.0) val = 0.0;
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
		case 0: cpu->v[rd].sf[0] = c + a * b; break;
		case 1: cpu->v[rd].sf[0] = c - a * b; break;
		case 2: cpu->v[rd].sf[0] = -(c + a * b); break;
		case 3: cpu->v[rd].sf[0] = -(c - a * b); break;
		}
	} else if (type == 1) {
		double a = cpu->v[rn].df[0];
		double b = cpu->v[rm].df[0];
		double c = cpu->v[ra].df[0];

		switch ((o1 << 1) | o0) {
		case 0: cpu->v[rd].df[0] = c + a * b; break;
		case 1: cpu->v[rd].df[0] = c - a * b; break;
		case 2: cpu->v[rd].df[0] = -(c + a * b); break;
		case 3: cpu->v[rd].df[0] = -(c - a * b); break;
		}
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

/* ---- SIMD copy operations ---- */

static int
exec_dup_general(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, imm5, rn, rd;
	uint64_t	val;
	int		i, elems;

	Q = bit(insn, 30);
	imm5 = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	val = cpu_xreg(cpu, rn);
	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (imm5 & 0x01) {
		elems = Q ? 16 : 8;
		for (i = 0; i < elems; i++)
			cpu->v[rd].b[i] = (uint8_t)val;
	} else if (imm5 & 0x02) {
		elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++)
			cpu->v[rd].h[i] = (uint16_t)val;
	} else if (imm5 & 0x04) {
		elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = (uint32_t)val;
	} else if (imm5 & 0x08) {
		elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++)
			cpu->v[rd].d[i] = val;
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

static int
exec_dup_element(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, imm5, rn, rd;
	int		i, elems;

	Q = bit(insn, 30);
	imm5 = bits(insn, 20, 16);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	if (imm5 & 1) {
		int idx = (imm5 >> 1) & 0xF;
		uint8_t val = cpu->v[rn].b[idx];
		elems = Q ? 16 : 8;
		for (i = 0; i < elems; i++)
			cpu->v[rd].b[i] = val;
	} else if (imm5 & 2) {
		int idx = (imm5 >> 2) & 0x7;
		uint16_t val = cpu->v[rn].h[idx];
		elems = Q ? 8 : 4;
		for (i = 0; i < elems; i++)
			cpu->v[rd].h[i] = val;
	} else if (imm5 & 4) {
		int idx = (imm5 >> 3) & 0x3;
		uint32_t val = cpu->v[rn].s[idx];
		elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++)
			cpu->v[rd].s[i] = val;
	} else if (imm5 & 8) {
		int idx = (imm5 >> 4) & 0x1;
		uint64_t val = cpu->v[rn].d[idx];
		elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++)
			cpu->v[rd].d[i] = val;
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

static int
exec_ins_element(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	imm5, imm4, rn, rd;

	imm5 = bits(insn, 20, 16);
	imm4 = bits(insn, 14, 11);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	if (imm5 & 1) {
		int dst_idx = (imm5 >> 1) & 0xF;
		int src_idx = (imm4) & 0xF;
		cpu->v[rd].b[dst_idx] = cpu->v[rn].b[src_idx];
	} else if (imm5 & 2) {
		int dst_idx = (imm5 >> 2) & 0x7;
		int src_idx = (imm4 >> 1) & 0x7;
		cpu->v[rd].h[dst_idx] = cpu->v[rn].h[src_idx];
	} else if (imm5 & 4) {
		int dst_idx = (imm5 >> 3) & 0x3;
		int src_idx = (imm4 >> 2) & 0x3;
		cpu->v[rd].s[dst_idx] = cpu->v[rn].s[src_idx];
	} else if (imm5 & 8) {
		int dst_idx = (imm5 >> 4) & 0x1;
		int src_idx = (imm4 >> 3) & 0x1;
		cpu->v[rd].d[dst_idx] = cpu->v[rn].d[src_idx];
	} else {
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

/* ---- Modified immediate: MOVI, MVNI, ORR imm, BIC imm ---- */

static int
exec_simd_mod_imm(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, op, cmode, rd;
	uint8_t		imm8;
	int		i;

	Q = bit(insn, 30);
	op = bit(insn, 29);
	cmode = bits(insn, 15, 12);
	rd = bits(insn, 4, 0);

	imm8 = (uint8_t)((bits(insn, 18, 16) << 5) | bits(insn, 9, 5));

	/* Determine operation: MOVI/MVNI/ORR/BIC based on cmode and op */
	uint32_t cmode_hi = cmode >> 1;
	int is_orr = 0, is_bic = 0;

	/* ORR immediate: op=0, cmode = 0x1/0x3/0x5/0x7 (odd cmode < 0xC) */
	/* BIC immediate: op=1, cmode = 0x1/0x3/0x5/0x7 */
	/* Actually: ORR is cmode 0001,0011,0101,0111 with specific patterns */
	/* For 32-bit: cmode=0b0xx1 => ORR (op=0), BIC (op=1) */
	/* For 16-bit: cmode=0b1001,0b1011 => ORR (op=0), BIC (op=1) */

	if (op == 0 && (cmode & 1) && cmode_hi < 4)
		is_orr = 1;
	if (op == 0 && cmode == 0x09)
		is_orr = 1;
	if (op == 0 && cmode == 0x0B)
		is_orr = 1;
	if (op == 1 && (cmode & 1) && cmode_hi < 4)
		is_bic = 1;
	if (op == 1 && cmode == 0x09)
		is_bic = 1;
	if (op == 1 && cmode == 0x0B)
		is_bic = 1;

	/* MOVI/MVNI first - clear destination */
	if (!is_orr && !is_bic)
		memset(&cpu->v[rd], 0, sizeof(vreg_t));

	/* 32-bit shifted: cmode 000x, 001x, 010x, 011x */
	if (cmode_hi < 4) {
		uint32_t shift = cmode_hi * 8;
		uint32_t val32 = (uint32_t)imm8 << shift;
		int elems = Q ? 4 : 2;

		if (is_orr) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] |= val32;
		} else if (is_bic) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] &= ~val32;
		} else if (op == 1) {
			/* MVNI */
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] = ~val32;
		} else {
			/* MOVI */
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] = val32;
		}
		return EMU_OK;
	}

	/* 16-bit shifted: cmode 100x, 101x */
	if (cmode_hi >= 4 && cmode_hi < 6) {
		uint32_t shift = (cmode_hi - 4) * 8;
		uint16_t val16 = (uint16_t)((uint16_t)imm8 << shift);
		int elems = Q ? 8 : 4;

		if (is_orr) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].h[i] |= val16;
		} else if (is_bic) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].h[i] &= ~val16;
		} else if (op == 1) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].h[i] = ~val16;
		} else {
			for (i = 0; i < elems; i++)
				cpu->v[rd].h[i] = val16;
		}
		return EMU_OK;
	}

	/* 32-bit MSL: cmode 110x */
	if (cmode_hi == 6) {
		uint32_t shift = (cmode & 1) ? 16 : 8;
		uint32_t val32 = ((uint32_t)imm8 << shift) |
		    ((1u << shift) - 1);
		int elems = Q ? 4 : 2;

		if (op == 1) {
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] = ~val32;
		} else {
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] = val32;
		}
		return EMU_OK;
	}

	/* 8-bit: cmode = 1110, op = 0 */
	if (cmode == 0x0E && op == 0) {
		int bytes = Q ? 16 : 8;
		for (i = 0; i < bytes; i++)
			cpu->v[rd].b[i] = imm8;
		return EMU_OK;
	}

	/* 64-bit: cmode = 1110, op = 1 */
	if (cmode == 0x0E && op == 1) {
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

	/* FMOV imm: cmode = 1111, op = 0 (single) or op = 1 (double, Q=1) */
	if (cmode == 0x0F) {
		if (op == 0) {
			/* Single-precision FMOV imm */
			uint32_t a = (imm8 >> 7) & 1;
			uint32_t b = (imm8 >> 6) & 1;
			uint32_t cdefgh = imm8 & 0x3F;
			uint32_t fbits = (a << 31) | ((b ? 0x3E : 0x40) << 24)
			    | (cdefgh << 17);
			int elems = Q ? 4 : 2;
			for (i = 0; i < elems; i++)
				cpu->v[rd].s[i] = fbits;
		} else {
			/* Double-precision FMOV imm (Q must be 1) */
			uint64_t a = (imm8 >> 7) & 1;
			uint64_t b = (imm8 >> 6) & 1;
			uint64_t cdefgh = imm8 & 0x3F;
			uint64_t fbits = (a << 63) |
			    ((b ? 0x0FFULL : 0x100ULL) << 51) |
			    (cdefgh << 46);
			cpu->v[rd].d[0] = fbits;
			if (Q)
				cpu->v[rd].d[1] = fbits;
		}
		return EMU_OK;
	}

	LOG_WARN("unimplemented MOVI op=%u cmode=0x%x at 0x%llx",
	    op, cmode, (unsigned long long)cpu->pc);
	return EMU_UNIMPL;
}

/* ---- SIMD load/store ---- */

static int
exec_simd_ldst_multi(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, L, rm, opcode, size, rn, rt;
	uint64_t	addr;
	int		bytes, post, regs, spacing;
	void		*host;

	Q = bit(insn, 30);
	L = bit(insn, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 12);
	size = bits(insn, 11, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);
	post = bit(insn, 23);

	bytes = Q ? 16 : 8;
	addr = cpu_xreg_sp(cpu, rn);

	/* Determine number of registers and structure type */
	switch (opcode) {
	case 0x07:	/* LD1/ST1 - 1 register */
		regs = 1; spacing = 1; break;
	case 0x0A:	/* LD1/ST1 - 2 registers */
		regs = 2; spacing = 1; break;
	case 0x06:	/* LD1/ST1 - 3 registers */
		regs = 3; spacing = 1; break;
	case 0x02:	/* LD1/ST1 - 4 registers */
		regs = 4; spacing = 1; break;
	case 0x08:	/* LD2/ST2 */
		regs = 2; spacing = 2; break;
	case 0x04:	/* LD3/ST3 */
		regs = 3; spacing = 3; break;
	case 0x00:	/* LD4/ST4 */
		regs = 4; spacing = 4; break;
	default:
		LOG_WARN("unimplemented SIMD ldst opcode=0x%x at 0x%llx",
		    opcode, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	(void)size;

	if (spacing == 1) {
		/* LD1/ST1 multiple consecutive registers */
		for (int r = 0; r < regs; r++) {
			uint32_t reg = (rt + r) & 31;
			uint64_t a = addr + (uint64_t)(r * bytes);

			if (L) {
				memset(&cpu->v[reg], 0, sizeof(vreg_t));
				host = mem_translate(cpu->mem, a,
				    (uint64_t)bytes, MEM_PROT_READ);
				if (host != NULL) {
					memcpy(&cpu->v[reg], host,
					    (size_t)bytes);
				} else {
					for (int i = 0; i < bytes; i++) {
						if (mem_read8(cpu->mem,
						    a + (uint64_t)i,
						    &cpu->v[reg].b[i]) != 0)
							return EMU_SEGFAULT;
					}
				}
			} else {
				host = mem_translate(cpu->mem, a,
				    (uint64_t)bytes, MEM_PROT_WRITE);
				if (host != NULL) {
					memcpy(host, &cpu->v[reg],
					    (size_t)bytes);
				} else {
					for (int i = 0; i < bytes; i++) {
						if (mem_write8(cpu->mem,
						    a + (uint64_t)i,
						    cpu->v[reg].b[i]) != 0)
							return EMU_SEGFAULT;
					}
				}
			}
		}
	} else {
		/* LD2/LD3/LD4 / ST2/ST3/ST4 - interleaved */
		int esize = 1 << size;
		int elems = bytes / esize;

		for (int r = 0; r < regs; r++) {
			uint32_t reg = (rt + r) & 31;
			if (L)
				memset(&cpu->v[reg], 0, sizeof(vreg_t));
		}

		for (int e = 0; e < elems; e++) {
			for (int r = 0; r < regs; r++) {
				uint32_t reg = (rt + r) & 31;
				uint64_t a = addr +
				    (uint64_t)((e * regs + r) * esize);

				if (L) {
					uint64_t tmp = 0;
					for (int b2 = 0; b2 < esize; b2++) {
						uint8_t byte;
						if (mem_read8(cpu->mem,
						    a + (uint64_t)b2,
						    &byte) != 0)
							return EMU_SEGFAULT;
						tmp |= (uint64_t)byte <<
						    (b2 * 8);
					}
					velem_set(&cpu->v[reg], size, e, tmp);
				} else {
					uint64_t tmp = velem_u(&cpu->v[reg],
					    size, e);
					for (int b2 = 0; b2 < esize; b2++) {
						if (mem_write8(cpu->mem,
						    a + (uint64_t)b2,
						    (uint8_t)(tmp >>
						    (b2 * 8))) != 0)
							return EMU_SEGFAULT;
					}
				}
			}
		}
	}

	/* Post-index writeback */
	if (post) {
		uint64_t total = (uint64_t)(regs * bytes);
		uint64_t offset;
		if (rm == 31)
			offset = total;
		else
			offset = cpu_xreg(cpu, rm);
		cpu_set_xreg_sp(cpu, rn, addr + offset);
	}

	return EMU_OK;
}

/* LD1R: load single element and replicate to all lanes */
static int
exec_simd_ld1r(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, rm, size, rn, rt;
	uint64_t	addr, val;
	int		post;

	Q = bit(insn, 30);
	rm = bits(insn, 20, 16);
	size = bits(insn, 11, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);
	post = bit(insn, 23);

	addr = cpu_xreg_sp(cpu, rn);
	val = 0;

	int esize = 1 << size;
	for (int b2 = 0; b2 < esize; b2++) {
		uint8_t byte;
		if (mem_read8(cpu->mem, addr + (uint64_t)b2, &byte) != 0)
			return EMU_SEGFAULT;
		val |= (uint64_t)byte << (b2 * 8);
	}

	memset(&cpu->v[rt], 0, sizeof(vreg_t));
	int elems = velem_count(size, Q);
	for (int i = 0; i < elems; i++)
		velem_set(&cpu->v[rt], size, i, val);

	if (post) {
		uint64_t offset;
		if (rm == 31)
			offset = (uint64_t)esize;
		else
			offset = cpu_xreg(cpu, rm);
		cpu_set_xreg_sp(cpu, rn, addr + offset);
	}

	return EMU_OK;
}

/* ---- Three-same vector arithmetic ---- */

static int
exec_three_same(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, size, rm, opcode, rn, rd;
	int		i, elems;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	size = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 11);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	elems = velem_count(size, Q);
	memset(&res, 0, sizeof(res));

	switch (opcode) {
	case 0x00:	/* SHADD/UHADD */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i, (a + b) >> 1);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)((a + b) >> 1));
			}
		}
		break;

	case 0x01:	/* SQADD/UQADD */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    usat_unsigned(a + b, bw));
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)sat_signed(a + b, bw));
			}
		}
		break;

	case 0x02:	/* SRHADD/URHADD */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i, (a + b + 1) >> 1);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)((a + b + 1) >> 1));
			}
		}
		break;

	case 0x03:	/* logic: AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF */
	{
		int bytes = Q ? 16 : 8;
		uint32_t opc2 = (size << 1) | U;

		switch (opc2) {
		case 0:	/* AND */
			for (i = 0; i < bytes; i++)
				res.b[i] = cpu->v[rn].b[i] &
				    cpu->v[rm].b[i];
			break;
		case 1:	/* BIC */
			for (i = 0; i < bytes; i++)
				res.b[i] = cpu->v[rn].b[i] &
				    ~cpu->v[rm].b[i];
			break;
		case 2:	/* ORR */
			for (i = 0; i < bytes; i++)
				res.b[i] = cpu->v[rn].b[i] |
				    cpu->v[rm].b[i];
			break;
		case 3:	/* ORN */
			for (i = 0; i < bytes; i++)
				res.b[i] = cpu->v[rn].b[i] |
				    ~cpu->v[rm].b[i];
			break;
		case 4:	/* EOR */
			for (i = 0; i < bytes; i++)
				res.b[i] = cpu->v[rn].b[i] ^
				    cpu->v[rm].b[i];
			break;
		case 5:	/* BSL */
			for (i = 0; i < bytes; i++)
				res.b[i] = (cpu->v[rd].b[i] &
				    cpu->v[rn].b[i]) |
				    (~cpu->v[rd].b[i] &
				    cpu->v[rm].b[i]);
			break;
		case 6:	/* BIT */
			for (i = 0; i < bytes; i++)
				res.b[i] = (cpu->v[rm].b[i] &
				    cpu->v[rn].b[i]) |
				    (~cpu->v[rm].b[i] &
				    cpu->v[rd].b[i]);
			break;
		case 7:	/* BIF */
			for (i = 0; i < bytes; i++)
				res.b[i] = (~cpu->v[rm].b[i] &
				    cpu->v[rn].b[i]) |
				    (cpu->v[rm].b[i] &
				    cpu->v[rd].b[i]);
			break;
		}
		cpu->v[rd] = res;
		return EMU_OK;
	}

	case 0x04:	/* SHSUB/UHSUB */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)((int64_t)(a - b) >> 1));
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)((a - b) >> 1));
			}
		}
		break;

	case 0x05:	/* SQSUB/UQSUB */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    a > b ? a - b : 0);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)sat_signed(a - b, bw));
			}
		}
		break;

	case 0x06:	/* CMGT/CMHI (register) */
		for (i = 0; i < elems; i++) {
			int cmp;
			if (U)
				cmp = velem_u(&cpu->v[rn], size, i) >
				    velem_u(&cpu->v[rm], size, i);
			else
				cmp = velem_s(&cpu->v[rn], size, i) >
				    velem_s(&cpu->v[rm], size, i);
			velem_set(&res, size, i,
			    cmp ? esize_mask(size) : 0);
		}
		break;

	case 0x07:	/* CMGE/CMHS (register) */
		for (i = 0; i < elems; i++) {
			int cmp;
			if (U)
				cmp = velem_u(&cpu->v[rn], size, i) >=
				    velem_u(&cpu->v[rm], size, i);
			else
				cmp = velem_s(&cpu->v[rn], size, i) >=
				    velem_s(&cpu->v[rm], size, i);
			velem_set(&res, size, i,
			    cmp ? esize_mask(size) : 0);
		}
		break;

	case 0x08:	/* SSHL/USHL */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			int64_t sh = velem_s(&cpu->v[rm], size, i);

			/* Clamp to [-bw, bw] */
			if (sh < -bw) sh = -bw;
			if (sh > bw) sh = bw;

			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i, a << sh);
				else
					velem_set(&res, size, i,
					    a >> (-sh));
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    (uint64_t)(a << sh));
				else
					velem_set(&res, size, i,
					    (uint64_t)(a >> (-sh)));
			}
		}
		break;

	case 0x09:	/* SQSHL/UQSHL (register) */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			int64_t sh = velem_s(&cpu->v[rm], size, i);

			if (sh < -bw) sh = -bw;
			if (sh > bw) sh = bw;

			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    usat_unsigned(a << sh, bw));
				else
					velem_set(&res, size, i,
					    a >> (-sh));
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    (uint64_t)sat_signed(
					    a << sh, bw));
				else
					velem_set(&res, size, i,
					    (uint64_t)(a >> (-sh)));
			}
		}
		break;

	case 0x0A:	/* SRSHL/URSHL */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			int64_t sh = velem_s(&cpu->v[rm], size, i);

			if (sh < -bw) sh = -bw;
			if (sh > bw) sh = bw;

			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i, a << sh);
				else {
					int neg_sh = (int)(-sh);
					uint64_t round = (neg_sh > 0) ?
					    (a >> (neg_sh - 1)) & 1 : 0;
					velem_set(&res, size, i,
					    (a >> neg_sh) + round);
				}
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    (uint64_t)(a << sh));
				else {
					int neg_sh = (int)(-sh);
					int64_t round = (neg_sh > 0) ?
					    (a >> (neg_sh - 1)) & 1 : 0;
					velem_set(&res, size, i,
					    (uint64_t)(
					    (a >> neg_sh) + round));
				}
			}
		}
		break;

	case 0x0B:	/* SQRSHL/UQRSHL */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			int64_t sh = velem_s(&cpu->v[rm], size, i);

			if (sh < -bw) sh = -bw;
			if (sh > bw) sh = bw;

			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    usat_unsigned(a << sh, bw));
				else {
					int neg_sh = (int)(-sh);
					uint64_t round = (neg_sh > 0) ?
					    (a >> (neg_sh - 1)) & 1 : 0;
					velem_set(&res, size, i,
					    (a >> neg_sh) + round);
				}
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				if (sh >= 0)
					velem_set(&res, size, i,
					    (uint64_t)sat_signed(
					    a << sh, bw));
				else {
					int neg_sh = (int)(-sh);
					int64_t round = (neg_sh > 0) ?
					    (a >> (neg_sh - 1)) & 1 : 0;
					velem_set(&res, size, i,
					    (uint64_t)(
					    (a >> neg_sh) + round));
				}
			}
		}
		break;

	case 0x0C:	/* SMAX/UMAX */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i, a > b ? a : b);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)(a > b ? a : b));
			}
		}
		break;

	case 0x0D:	/* SMIN/UMIN */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i, a < b ? a : b);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    (uint64_t)(a < b ? a : b));
			}
		}
		break;

	case 0x0E:	/* SABD/UABD */
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i,
				    a > b ? a - b : b - a);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				int64_t d = a - b;
				velem_set(&res, size, i,
				    (uint64_t)(d < 0 ? -d : d));
			}
		}
		break;

	case 0x0F:	/* SABA/UABA */
		for (i = 0; i < elems; i++) {
			uint64_t acc = velem_u(&cpu->v[rd], size, i);
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				uint64_t d = a > b ? a - b : b - a;
				velem_set(&res, size, i, acc + d);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, i);
				int64_t b = velem_s(&cpu->v[rm], size, i);
				int64_t d = a - b;
				if (d < 0) d = -d;
				velem_set(&res, size, i,
				    acc + (uint64_t)d);
			}
		}
		break;

	case 0x10:	/* ADD/SUB (vector) */
		for (i = 0; i < elems; i++) {
			uint64_t a = velem_u(&cpu->v[rn], size, i);
			uint64_t b = velem_u(&cpu->v[rm], size, i);
			velem_set(&res, size, i, U ? a - b : a + b);
		}
		break;

	case 0x11:	/* CMTST/CMEQ (register) */
		for (i = 0; i < elems; i++) {
			uint64_t a = velem_u(&cpu->v[rn], size, i);
			uint64_t b = velem_u(&cpu->v[rm], size, i);
			int cmp;
			if (U)
				cmp = (a == b);
			else
				cmp = ((a & b) != 0);
			velem_set(&res, size, i,
			    cmp ? esize_mask(size) : 0);
		}
		break;

	case 0x12:	/* MLA/MLS (vector) */
		for (i = 0; i < elems; i++) {
			uint64_t a = velem_u(&cpu->v[rn], size, i);
			uint64_t b = velem_u(&cpu->v[rm], size, i);
			uint64_t d = velem_u(&cpu->v[rd], size, i);
			if (U)
				velem_set(&res, size, i, d - a * b);
			else
				velem_set(&res, size, i, d + a * b);
		}
		break;

	case 0x13:	/* MUL/PMUL */
		if (U) {
			/* PMUL (polynomial) - only for size=0 (bytes) */
			for (i = 0; i < elems; i++) {
				uint8_t a = cpu->v[rn].b[i];
				uint8_t b = cpu->v[rm].b[i];
				uint8_t r = 0;
				for (int j = 0; j < 8; j++) {
					if (b & (1 << j))
						r ^= a << j;
				}
				res.b[i] = r;
			}
		} else {
			/* MUL */
			for (i = 0; i < elems; i++) {
				uint64_t a = velem_u(&cpu->v[rn], size, i);
				uint64_t b = velem_u(&cpu->v[rm], size, i);
				velem_set(&res, size, i, a * b);
			}
		}
		break;

	case 0x14:	/* SMAXP/UMAXP */
		for (i = 0; i < elems / 2; i++) {
			if (U) {
				uint64_t a0 = velem_u(&cpu->v[rn],
				    size, 2 * i);
				uint64_t a1 = velem_u(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, size, i,
				    a0 > a1 ? a0 : a1);
			} else {
				int64_t a0 = velem_s(&cpu->v[rn],
				    size, 2 * i);
				int64_t a1 = velem_s(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, size, i,
				    (uint64_t)(a0 > a1 ? a0 : a1));
			}
		}
		for (i = 0; i < elems / 2; i++) {
			if (U) {
				uint64_t b0 = velem_u(&cpu->v[rm],
				    size, 2 * i);
				uint64_t b1 = velem_u(&cpu->v[rm],
				    size, 2 * i + 1);
				velem_set(&res, size, elems / 2 + i,
				    b0 > b1 ? b0 : b1);
			} else {
				int64_t b0 = velem_s(&cpu->v[rm],
				    size, 2 * i);
				int64_t b1 = velem_s(&cpu->v[rm],
				    size, 2 * i + 1);
				velem_set(&res, size, elems / 2 + i,
				    (uint64_t)(b0 > b1 ? b0 : b1));
			}
		}
		break;

	case 0x15:	/* SMINP/UMINP */
		for (i = 0; i < elems / 2; i++) {
			if (U) {
				uint64_t a0 = velem_u(&cpu->v[rn],
				    size, 2 * i);
				uint64_t a1 = velem_u(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, size, i,
				    a0 < a1 ? a0 : a1);
			} else {
				int64_t a0 = velem_s(&cpu->v[rn],
				    size, 2 * i);
				int64_t a1 = velem_s(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, size, i,
				    (uint64_t)(a0 < a1 ? a0 : a1));
			}
		}
		for (i = 0; i < elems / 2; i++) {
			if (U) {
				uint64_t b0 = velem_u(&cpu->v[rm],
				    size, 2 * i);
				uint64_t b1 = velem_u(&cpu->v[rm],
				    size, 2 * i + 1);
				velem_set(&res, size, elems / 2 + i,
				    b0 < b1 ? b0 : b1);
			} else {
				int64_t b0 = velem_s(&cpu->v[rm],
				    size, 2 * i);
				int64_t b1 = velem_s(&cpu->v[rm],
				    size, 2 * i + 1);
				velem_set(&res, size, elems / 2 + i,
				    (uint64_t)(b0 < b1 ? b0 : b1));
			}
		}
		break;

	case 0x16:	/* SQDMULH/SQRDMULH */
	{
		for (i = 0; i < elems; i++) {
			int64_t a = velem_s(&cpu->v[rn], size, i);
			int64_t b = velem_s(&cpu->v[rm], size, i);
			int bw = esize_bits(size);
			int64_t prod = a * b;

			if (U) {
				/* SQRDMULH: round */
				int64_t round = (int64_t)1 << (bw - 2);
				velem_set(&res, size, i,
				    (uint64_t)sat_signed(
				    (prod + round) >> (bw - 1), bw));
			} else {
				/* SQDMULH */
				velem_set(&res, size, i,
				    (uint64_t)sat_signed(
				    prod >> (bw - 1), bw));
			}
		}
		break;
	}

	case 0x17:	/* ADDP */
		for (i = 0; i < elems / 2; i++) {
			uint64_t a0 = velem_u(&cpu->v[rn], size, 2 * i);
			uint64_t a1 = velem_u(&cpu->v[rn], size, 2 * i + 1);
			velem_set(&res, size, i, a0 + a1);
		}
		for (i = 0; i < elems / 2; i++) {
			uint64_t b0 = velem_u(&cpu->v[rm], size, 2 * i);
			uint64_t b1 = velem_u(&cpu->v[rm], size, 2 * i + 1);
			velem_set(&res, size, elems / 2 + i, b0 + b1);
		}
		break;

	default:
		LOG_WARN("unimplemented three-same opcode=0x%02x U=%u "
		    "size=%u at 0x%llx", opcode, U, size,
		    (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Three-same FP (size field encodes float type) ---- */

static int
exec_three_same_fp(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, sz, rm, opcode, rn, rd;
	vreg_t		res;
	int		i;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	sz = bit(insn, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 11);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&res, 0, sizeof(res));

	if (sz == 0) {
		/* Single precision */
		int elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++) {
			float a = cpu->v[rn].sf[i];
			float b = cpu->v[rm].sf[i];

			switch (opcode) {
			case 0x03:	/* FMUL/FMULX */
				res.sf[i] = a * b;
				break;
			case 0x04:	/* FCMEQ (U=0) / FCMGE (U=1) */
				if (U)
					res.s[i] = (a >= b) ?
					    0xFFFFFFFF : 0;
				else
					res.s[i] = (a == b) ?
					    0xFFFFFFFF : 0;
				break;
			case 0x05:	/* FMLA (U=0) / FMLS (U=1) */
				if (U)
					res.sf[i] = cpu->v[rd].sf[i] -
					    a * b;
				else
					res.sf[i] = cpu->v[rd].sf[i] +
					    a * b;
				break;
			case 0x06:	/* FMAX/FMAXNM (U=0) / FMIN/FMINNM (U=1) depends on bit23 */
				/* FMAXNM: U=0 sz=0 opcode=0x06 */
				/* FMINNM: U=0 sz=1 opcode=0x06 */
				/* For sz=0 here: FMAXNM (U=0) / FADDP will be separate */
				if (U)
					res.sf[i] = fmaxf(a, b);
				else
					res.sf[i] = isnan(a) ? b :
					    isnan(b) ? a : fmaxf(a, b);
				break;
			case 0x07:	/* FRECPS (U=0) / FRSQRTS (U=1) */
				if (U)
					res.sf[i] = (3.0f - a * b) / 2.0f;
				else
					res.sf[i] = 2.0f - a * b;
				break;
			case 0x0C:	/* FMAX (U=0) / FMIN (U=1) */
				if (U)
					res.sf[i] = fminf(a, b);
				else
					res.sf[i] = fmaxf(a, b);
				break;
			case 0x0D:	/* FMINNM (U=0) / FMAXNM variant */
				if (U)
					res.sf[i] = isnan(a) ? b :
					    isnan(b) ? a : fminf(a, b);
				else
					res.sf[i] = isnan(a) ? b :
					    isnan(b) ? a : fminf(a, b);
				break;
			case 0x0E:	/* FSUB (U=0) / FABD (U=1) */
				if (U)
					res.sf[i] = fabsf(a - b);
				else
					res.sf[i] = a - b;
				break;
			case 0x0F:	/* FCMGT (U=1) */
				if (U)
					res.s[i] = (a > b) ?
					    0xFFFFFFFF : 0;
				else
					return EMU_UNIMPL;
				break;
			case 0x1A:	/* FADD (U=0) / FADDP (U=1) */
				if (U) {
					/* FADDP */
					int half = elems / 2;
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					for (int p = 0; p < half; p++)
						tmp.sf[p] =
						    cpu->v[rn].sf[2*p] +
						    cpu->v[rn].sf[2*p+1];
					for (int p = 0; p < half; p++)
						tmp.sf[half+p] =
						    cpu->v[rm].sf[2*p] +
						    cpu->v[rm].sf[2*p+1];
					cpu->v[rd] = tmp;
					return EMU_OK;
				}
				res.sf[i] = a + b;
				break;
			case 0x1B:	/* FMUL */
				res.sf[i] = a * b;
				break;
			case 0x1C:	/* FCMGE (U=0) / FCMGT (U=1) */
				if (U)
					res.s[i] = (a > b) ?
					    0xFFFFFFFF : 0;
				else
					res.s[i] = (a >= b) ?
					    0xFFFFFFFF : 0;
				break;
			case 0x1D:	/* FACGE / FACGT */
				if (U)
					res.s[i] = (fabsf(a) > fabsf(b)) ?
					    0xFFFFFFFF : 0;
				else
					res.s[i] = (fabsf(a) >= fabsf(b)) ?
					    0xFFFFFFFF : 0;
				break;
			case 0x1E:	/* FMAX/FMIN pairwise */
				if (U) {
					/* FMINP */
					int half = elems / 2;
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					for (int p = 0; p < half; p++)
						tmp.sf[p] = fminf(
						    cpu->v[rn].sf[2*p],
						    cpu->v[rn].sf[2*p+1]);
					for (int p = 0; p < half; p++)
						tmp.sf[half+p] = fminf(
						    cpu->v[rm].sf[2*p],
						    cpu->v[rm].sf[2*p+1]);
					cpu->v[rd] = tmp;
					return EMU_OK;
				} else {
					/* FMAXP */
					int half = elems / 2;
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					for (int p = 0; p < half; p++)
						tmp.sf[p] = fmaxf(
						    cpu->v[rn].sf[2*p],
						    cpu->v[rn].sf[2*p+1]);
					for (int p = 0; p < half; p++)
						tmp.sf[half+p] = fmaxf(
						    cpu->v[rm].sf[2*p],
						    cpu->v[rm].sf[2*p+1]);
					cpu->v[rd] = tmp;
					return EMU_OK;
				}
			case 0x1F:	/* FDIV */
				res.sf[i] = a / b;
				break;
			default:
				LOG_WARN("unimplemented fp3same opcode=0x%02x "
				    "U=%u sz=%u at 0x%llx", opcode, U, sz,
				    (unsigned long long)cpu->pc);
				return EMU_UNIMPL;
			}
		}
	} else {
		/* Double precision */
		int elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++) {
			double a = cpu->v[rn].df[i];
			double b = cpu->v[rm].df[i];

			switch (opcode) {
			case 0x03:
				res.df[i] = a * b;
				break;
			case 0x04:
				if (U)
					res.d[i] = (a >= b) ? ~0ULL : 0;
				else
					res.d[i] = (a == b) ? ~0ULL : 0;
				break;
			case 0x05:
				if (U)
					res.df[i] = cpu->v[rd].df[i] -
					    a * b;
				else
					res.df[i] = cpu->v[rd].df[i] +
					    a * b;
				break;
			case 0x06:
				if (U)
					res.df[i] = fmax(a, b);
				else
					res.df[i] = isnan(a) ? b :
					    isnan(b) ? a : fmax(a, b);
				break;
			case 0x07:
				if (U)
					res.df[i] = (3.0 - a * b) / 2.0;
				else
					res.df[i] = 2.0 - a * b;
				break;
			case 0x0C:
				if (U)
					res.df[i] = fmin(a, b);
				else
					res.df[i] = fmax(a, b);
				break;
			case 0x0D:
				if (U)
					res.df[i] = isnan(a) ? b :
					    isnan(b) ? a : fmin(a, b);
				else
					res.df[i] = isnan(a) ? b :
					    isnan(b) ? a : fmin(a, b);
				break;
			case 0x0E:
				if (U)
					res.df[i] = fabs(a - b);
				else
					res.df[i] = a - b;
				break;
			case 0x0F:
				if (U)
					res.d[i] = (a > b) ? ~0ULL : 0;
				else
					return EMU_UNIMPL;
				break;
			case 0x1A:
				if (U) {
					int half = elems / 2;
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					if (half < 1) half = 1;
					tmp.df[0] = cpu->v[rn].df[0] +
					    cpu->v[rn].df[1];
					if (Q)
						tmp.df[1] =
						    cpu->v[rm].df[0] +
						    cpu->v[rm].df[1];
					cpu->v[rd] = tmp;
					return EMU_OK;
				}
				res.df[i] = a + b;
				break;
			case 0x1B:
				res.df[i] = a * b;
				break;
			case 0x1C:
				if (U)
					res.d[i] = (a > b) ? ~0ULL : 0;
				else
					res.d[i] = (a >= b) ? ~0ULL : 0;
				break;
			case 0x1D:
				if (U)
					res.d[i] = (fabs(a) > fabs(b)) ?
					    ~0ULL : 0;
				else
					res.d[i] = (fabs(a) >= fabs(b)) ?
					    ~0ULL : 0;
				break;
			case 0x1E:
				if (U) {
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					tmp.df[0] = fmin(cpu->v[rn].df[0],
					    cpu->v[rn].df[1]);
					if (Q)
						tmp.df[1] = fmin(
						    cpu->v[rm].df[0],
						    cpu->v[rm].df[1]);
					cpu->v[rd] = tmp;
					return EMU_OK;
				} else {
					vreg_t tmp;
					memset(&tmp, 0, sizeof(tmp));
					tmp.df[0] = fmax(cpu->v[rn].df[0],
					    cpu->v[rn].df[1]);
					if (Q)
						tmp.df[1] = fmax(
						    cpu->v[rm].df[0],
						    cpu->v[rm].df[1]);
					cpu->v[rd] = tmp;
					return EMU_OK;
				}
			case 0x1F:
				res.df[i] = a / b;
				break;
			default:
				LOG_WARN("unimplemented fp3same opcode=0x%02x "
				    "U=%u sz=%u at 0x%llx", opcode, U, sz,
				    (unsigned long long)cpu->pc);
				return EMU_UNIMPL;
			}
		}
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Three-different (widening/narrowing) ---- */

static int
exec_three_diff(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, size, rm, opcode, rn, rd;
	int		i, part;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	size = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 15, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	part = Q;	/* 0 = lower, 1 = upper */
	memset(&res, 0, sizeof(res));

	switch (opcode) {
	case 0x00:	/* SADDL/UADDL */
	case 0x02:	/* SSUBL/USUBL */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			int64_t a, b;
			if (U) {
				a = (int64_t)velem_u(&cpu->v[rn], size, si);
				b = (int64_t)velem_u(&cpu->v[rm], size, si);
			} else {
				a = velem_s(&cpu->v[rn], size, si);
				b = velem_s(&cpu->v[rm], size, si);
			}
			int64_t r = (opcode == 0x00) ? a + b : a - b;
			velem_set(&res, dsize, i, (uint64_t)r);
		}
		break;
	}

	case 0x01:	/* SADDW/UADDW */
	case 0x03:	/* SSUBW/USUBW */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			int64_t a, b;
			if (U) {
				a = (int64_t)velem_u(&cpu->v[rn], dsize, i);
				b = (int64_t)velem_u(&cpu->v[rm], size, si);
			} else {
				a = velem_s(&cpu->v[rn], dsize, i);
				b = velem_s(&cpu->v[rm], size, si);
			}
			int64_t r = (opcode == 0x01) ? a + b : a - b;
			velem_set(&res, dsize, i, (uint64_t)r);
		}
		break;
	}

	case 0x04:	/* ADDHN/RADDHN */
	case 0x06:	/* SUBHN/RSUBHN */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		int bw = esize_bits(dsize);

		if (part) res = cpu->v[rd];

		for (i = 0; i < elems; i++) {
			uint64_t a = velem_u(&cpu->v[rn], dsize, i);
			uint64_t b = velem_u(&cpu->v[rm], dsize, i);
			uint64_t r;

			if (opcode == 0x04)
				r = a + b;
			else
				r = a - b;

			/* RADDHN/RSUBHN: add rounding bit */
			if (U)
				r += (uint64_t)1 << (bw / 2 - 1);

			/* High half */
			r >>= (bw / 2);
			velem_set(&res, size, part * elems + i, r);
		}
		break;
	}

	case 0x05:	/* SABAL/UABAL */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		res = cpu->v[rd];
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			int64_t diff;
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, si);
				uint64_t b = velem_u(&cpu->v[rm], size, si);
				diff = (int64_t)(a > b ? a - b : b - a);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, si);
				int64_t b = velem_s(&cpu->v[rm], size, si);
				diff = a - b;
				if (diff < 0) diff = -diff;
			}
			uint64_t acc = velem_u(&cpu->v[rd], dsize, i);
			velem_set(&res, dsize, i, acc + (uint64_t)diff);
		}
		break;
	}

	case 0x07:	/* SABDL/UABDL */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			int64_t diff;
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, si);
				uint64_t b = velem_u(&cpu->v[rm], size, si);
				diff = (int64_t)(a > b ? a - b : b - a);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, si);
				int64_t b = velem_s(&cpu->v[rm], size, si);
				diff = a - b;
				if (diff < 0) diff = -diff;
			}
			velem_set(&res, dsize, i, (uint64_t)diff);
		}
		break;
	}

	case 0x08:	/* SMLAL/UMLAL */
	case 0x0A:	/* SMLSL/UMLSL */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		res = cpu->v[rd];
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			int64_t a, b;
			if (U) {
				a = (int64_t)velem_u(&cpu->v[rn], size, si);
				b = (int64_t)velem_u(&cpu->v[rm], size, si);
			} else {
				a = velem_s(&cpu->v[rn], size, si);
				b = velem_s(&cpu->v[rm], size, si);
			}
			int64_t prod = a * b;
			int64_t acc = (int64_t)velem_u(&cpu->v[rd], dsize, i);
			int64_t r = (opcode == 0x08) ?
			    acc + prod : acc - prod;
			velem_set(&res, dsize, i, (uint64_t)r);
		}
		break;
	}

	case 0x0C:	/* SMULL/UMULL */
	{
		int dsize = size + 1;
		int elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;
		for (i = 0; i < elems; i++) {
			int si = part * elems + i;
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn], size, si);
				uint64_t b = velem_u(&cpu->v[rm], size, si);
				velem_set(&res, dsize, i, a * b);
			} else {
				int64_t a = velem_s(&cpu->v[rn], size, si);
				int64_t b = velem_s(&cpu->v[rm], size, si);
				velem_set(&res, dsize, i,
				    (uint64_t)(a * b));
			}
		}
		break;
	}

	case 0x0E:	/* PMULL/PMULL2 (U=0, size=0 only) */
	{
		/* Polynomial multiply long 8x8->16 */
		if (size != 0) {
			LOG_WARN("PMULL with size=%u not supported", size);
			return EMU_UNIMPL;
		}
		for (i = 0; i < 8; i++) {
			int si = part * 8 + i;
			uint8_t a = cpu->v[rn].b[si];
			uint8_t b = cpu->v[rm].b[si];
			uint16_t r = 0;
			for (int j = 0; j < 8; j++) {
				if (b & (1 << j))
					r ^= (uint16_t)a << j;
			}
			res.h[i] = r;
		}
		break;
	}

	default:
		LOG_WARN("unimplemented three-diff opcode=0x%x U=%u at 0x%llx",
		    opcode, U, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Two-reg misc ---- */

static int
exec_two_reg_misc(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, size, opcode, rn, rd;
	int		i, elems;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	size = bits(insn, 23, 22);
	opcode = bits(insn, 16, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	elems = velem_count(size, Q);
	memset(&res, 0, sizeof(res));

	switch (opcode) {
	case 0x00:	/* REV64 (U=0) / REV32 (U=1) */
		if (U) {
			/* REV32: reverse elements within 32-bit groups */
			int per_group = 4 >> size;
			int groups = (Q ? 16 : 8) / 4;
			for (int g = 0; g < groups; g++) {
				for (i = 0; i < per_group; i++) {
					int si = g * per_group + i;
					int di = g * per_group +
					    (per_group - 1 - i);
					velem_set(&res, size, di,
					    velem_u(&cpu->v[rn], size, si));
				}
			}
		} else {
			/* REV64: reverse elements within 64-bit groups */
			int per_group = 8 >> size;
			int groups = (Q ? 16 : 8) / 8;
			for (int g = 0; g < groups; g++) {
				for (i = 0; i < per_group; i++) {
					int si = g * per_group + i;
					int di = g * per_group +
					    (per_group - 1 - i);
					velem_set(&res, size, di,
					    velem_u(&cpu->v[rn], size, si));
				}
			}
		}
		break;

	case 0x01:	/* REV16 (U=0) */
		if (!U && size == 0) {
			int bytes = Q ? 16 : 8;
			for (i = 0; i < bytes; i += 2) {
				res.b[i] = cpu->v[rn].b[i + 1];
				res.b[i + 1] = cpu->v[rn].b[i];
			}
		} else {
			return EMU_UNIMPL;
		}
		break;

	case 0x02:	/* SADDLP/UADDLP */
	{
		int dsize = size + 1;
		int src_elems = velem_count(size, Q);
		int dst_elems = src_elems / 2;
		for (i = 0; i < dst_elems; i++) {
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn],
				    size, 2 * i);
				uint64_t b = velem_u(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, dsize, i, a + b);
			} else {
				int64_t a = velem_s(&cpu->v[rn],
				    size, 2 * i);
				int64_t b = velem_s(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, dsize, i,
				    (uint64_t)(a + b));
			}
		}
		break;
	}

	case 0x03:	/* SUQADD (U=0) / USQADD (U=1) */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			if (U) {
				/* USQADD: unsigned + signed */
				uint64_t a = velem_u(&cpu->v[rd], size, i);
				int64_t b = velem_s(&cpu->v[rn], size, i);
				int64_t r = (int64_t)a + b;
				velem_set(&res, size, i,
				    sat_unsigned(r, bw));
			} else {
				/* SUQADD: signed + unsigned */
				int64_t a = velem_s(&cpu->v[rd], size, i);
				uint64_t b = velem_u(&cpu->v[rn], size, i);
				int64_t r = a + (int64_t)b;
				velem_set(&res, size, i,
				    (uint64_t)sat_signed(r, bw));
			}
		}
		break;

	case 0x04:	/* CLS (U=0) / CLZ (U=1) */
		for (i = 0; i < elems; i++) {
			uint64_t val = velem_u(&cpu->v[rn], size, i);
			int bw = esize_bits(size);
			if (U)
				velem_set(&res, size, i,
				    (uint64_t)clz_n(val, bw));
			else
				velem_set(&res, size, i,
				    (uint64_t)cls_n(val, bw));
		}
		break;

	case 0x05:	/* CNT (U=0, size=0) / NOT (U=1, size=0) / RBIT (U=1, size=1) */
		if (!U && size == 0) {
			/* CNT */
			int bytes = Q ? 16 : 8;
			for (i = 0; i < bytes; i++) {
				uint8_t v = cpu->v[rn].b[i];
				uint8_t cnt = 0;
				while (v) {
					cnt += v & 1;
					v >>= 1;
				}
				res.b[i] = cnt;
			}
		} else if (U && size == 0) {
			/* NOT/MVN */
			int bytes = Q ? 16 : 8;
			for (i = 0; i < bytes; i++)
				res.b[i] = ~cpu->v[rn].b[i];
		} else if (U && size == 1) {
			/* RBIT */
			int bytes = Q ? 16 : 8;
			for (i = 0; i < bytes; i++)
				res.b[i] = rbit8(cpu->v[rn].b[i]);
		} else {
			return EMU_UNIMPL;
		}
		break;

	case 0x06:	/* SADALP/UADALP */
	{
		int dsize = size + 1;
		int src_elems = velem_count(size, Q);
		int dst_elems = src_elems / 2;
		for (i = 0; i < dst_elems; i++) {
			uint64_t acc = velem_u(&cpu->v[rd], dsize, i);
			if (U) {
				uint64_t a = velem_u(&cpu->v[rn],
				    size, 2 * i);
				uint64_t b = velem_u(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, dsize, i, acc + a + b);
			} else {
				int64_t a = velem_s(&cpu->v[rn],
				    size, 2 * i);
				int64_t b = velem_s(&cpu->v[rn],
				    size, 2 * i + 1);
				velem_set(&res, dsize, i,
				    (uint64_t)((int64_t)acc + a + b));
			}
		}
		break;
	}

	case 0x07:	/* SQABS (U=0) / SQNEG (U=1) */
		for (i = 0; i < elems; i++) {
			int bw = esize_bits(size);
			int64_t val = velem_s(&cpu->v[rn], size, i);
			if (U)
				val = -val;
			else
				val = val < 0 ? -val : val;
			velem_set(&res, size, i,
			    (uint64_t)sat_signed(val, bw));
		}
		break;

	case 0x08:	/* CMGT zero (U=0) / CMGE zero (U=1) */
		for (i = 0; i < elems; i++) {
			int64_t val = velem_s(&cpu->v[rn], size, i);
			int cmp = U ? (val >= 0) : (val > 0);
			velem_set(&res, size, i,
			    cmp ? esize_mask(size) : 0);
		}
		break;

	case 0x09:	/* CMEQ zero (U=0) / CMLE zero (U=1) */
		for (i = 0; i < elems; i++) {
			int64_t val = velem_s(&cpu->v[rn], size, i);
			int cmp = U ? (val <= 0) : (val == 0);
			velem_set(&res, size, i,
			    cmp ? esize_mask(size) : 0);
		}
		break;

	case 0x0A:	/* CMLT zero (U=0) / ABS (U=1) */
		for (i = 0; i < elems; i++) {
			int64_t val = velem_s(&cpu->v[rn], size, i);
			if (U) {
				/* ABS */
				velem_set(&res, size, i,
				    (uint64_t)(val < 0 ? -val : val));
			} else {
				/* CMLT zero */
				int cmp = (val < 0);
				velem_set(&res, size, i,
				    cmp ? esize_mask(size) : 0);
			}
		}
		break;

	case 0x0B:	/* NEG (U=1) */
		if (U) {
			for (i = 0; i < elems; i++) {
				uint64_t val = velem_u(&cpu->v[rn], size, i);
				velem_set(&res, size, i,
				    (~val + 1) & esize_mask(size));
			}
		} else {
			return EMU_UNIMPL;
		}
		break;

	case 0x12:	/* XTN/XTN2 (U=0) / SQXTUN (U=1) */
	{
		int part = Q;
		int src_size = size + 1;
		int src_elems = (src_size == 1) ? 8 :
		    (src_size == 2) ? 4 : 2;

		if (part) res = cpu->v[rd];

		for (i = 0; i < src_elems; i++) {
			if (U) {
				/* SQXTUN: saturating signed to unsigned */
				int64_t val = velem_s(&cpu->v[rn],
				    src_size, i);
				int bw = esize_bits(size);
				velem_set(&res, size,
				    part * src_elems + i,
				    sat_unsigned(val, bw));
			} else {
				/* XTN: truncate */
				uint64_t val = velem_u(&cpu->v[rn],
				    src_size, i);
				velem_set(&res, size,
				    part * src_elems + i, val);
			}
		}
		break;
	}

	case 0x14:	/* SQXTN (U=0) / UQXTN (U=1) */
	{
		int part = Q;
		int src_size = size + 1;
		int src_elems = (src_size == 1) ? 8 :
		    (src_size == 2) ? 4 : 2;
		int bw = esize_bits(size);

		if (part) res = cpu->v[rd];

		for (i = 0; i < src_elems; i++) {
			if (U) {
				uint64_t val = velem_u(&cpu->v[rn],
				    src_size, i);
				velem_set(&res, size,
				    part * src_elems + i,
				    usat_unsigned(val, bw));
			} else {
				int64_t val = velem_s(&cpu->v[rn],
				    src_size, i);
				velem_set(&res, size,
				    part * src_elems + i,
				    (uint64_t)sat_signed(val, bw));
			}
		}
		break;
	}

	default:
		LOG_WARN("unimplemented two-reg-misc opcode=0x%02x U=%u "
		    "size=%u at 0x%llx", opcode, U, size,
		    (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Two-reg misc FP ---- */

static int
exec_two_reg_misc_fp(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, sz, opcode, rn, rd;
	int		i;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	sz = bit(insn, 22);
	opcode = bits(insn, 16, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&res, 0, sizeof(res));

	if (sz == 0) {
		/* Single precision */
		int elems = Q ? 4 : 2;
		for (i = 0; i < elems; i++) {
			float val = cpu->v[rn].sf[i];

			switch (opcode) {
			case 0x0C:	/* FCMGT zero(U=0) / FCMGE zero(U=1) */
				if (U)
					res.s[i] = (val >= 0.0f) ?
					    0xFFFFFFFF : 0;
				else
					res.s[i] = (val > 0.0f) ?
					    0xFFFFFFFF : 0;
				break;
			case 0x0D:	/* FCMEQ zero(U=0) / FCMLE zero(U=1) */
				if (U)
					res.s[i] = (val <= 0.0f) ?
					    0xFFFFFFFF : 0;
				else
					res.s[i] = (val == 0.0f) ?
					    0xFFFFFFFF : 0;
				break;
			case 0x0E:	/* FCMLT zero (U=0) */
				if (!U)
					res.s[i] = (val < 0.0f) ?
					    0xFFFFFFFF : 0;
				else
					return EMU_UNIMPL;
				break;
			case 0x0F:	/* FABS (U=0) / FNEG (U=1) */
				if (U)
					res.sf[i] = -val;
				else
					res.sf[i] = fabsf(val);
				break;
			case 0x19:	/* FRINTM(U=0) / FRINTP(U=0 a=1) ... */
				/* FRINTI/FRINTX/FRINTA/FRINTN/FRINTM/FRINTP/FRINTZ */
				if (U)
					res.sf[i] = truncf(val);
				else
					res.sf[i] = floorf(val);
				break;
			case 0x18:	/* FRINTN (U=0) / FRINTA (U=1) */
				if (U)
					res.sf[i] = roundf(val);
				else
					res.sf[i] = rintf(val);
				break;
			case 0x1A:	/* FCVTNS/FCVTNU/FCVTMS/FCVTMU... */
				if (U)
					res.s[i] = (uint32_t)rintf(val);
				else
					res.s[i] = (uint32_t)(int32_t)
					    rintf(val);
				break;
			case 0x1B:	/* FCVTMU(U=1) / FCVTMS(U=0) */
				if (U)
					res.s[i] = (val < 0.0f) ? 0 :
					    (uint32_t)floorf(val);
				else
					res.s[i] = (uint32_t)(int32_t)
					    floorf(val);
				break;
			case 0x1C:	/* FCVTAS(U=0) / FCVTAU(U=1) */
				if (U) {
					float rv = roundf(val);
					res.s[i] = (rv < 0.0f) ? 0 :
					    (uint32_t)rv;
				} else
					res.s[i] = (uint32_t)(int32_t)
					    roundf(val);
				break;
			case 0x1D:	/* SCVTF(U=0) / UCVTF(U=1) */
				if (U)
					res.sf[i] = (float)cpu->v[rn].s[i];
				else
					res.sf[i] = (float)
					    (int32_t)cpu->v[rn].s[i];
				break;
			case 0x1E:	/* FCVTPS(U=0)/FCVTPU(U=1) or FCVTZS/FCVTZU */
				if (U) {
					float rv = ceilf(val);
					res.s[i] = (rv < 0.0f) ? 0 :
					    (uint32_t)rv;
				} else
					res.s[i] = (uint32_t)(int32_t)
					    ceilf(val);
				break;
			case 0x1F:	/* FSQRT (U=1) / FRECPE (U=0) */
				if (U)
					res.sf[i] = sqrtf(val);
				else
					res.sf[i] = 1.0f / val;
				break;
			case 0x17:	/* FCVTN / FCVTL */
				/* Handled separately */
				return EMU_UNIMPL;
			default:
				LOG_WARN("unimpl fp 2reg misc opc=0x%x U=%u",
				    opcode, U);
				return EMU_UNIMPL;
			}
		}
	} else {
		/* Double precision */
		int elems = Q ? 2 : 1;
		for (i = 0; i < elems; i++) {
			double val = cpu->v[rn].df[i];

			switch (opcode) {
			case 0x0C:
				if (U)
					res.d[i] = (val >= 0.0) ? ~0ULL : 0;
				else
					res.d[i] = (val > 0.0) ? ~0ULL : 0;
				break;
			case 0x0D:
				if (U)
					res.d[i] = (val <= 0.0) ? ~0ULL : 0;
				else
					res.d[i] = (val == 0.0) ? ~0ULL : 0;
				break;
			case 0x0E:
				if (!U)
					res.d[i] = (val < 0.0) ? ~0ULL : 0;
				else
					return EMU_UNIMPL;
				break;
			case 0x0F:
				if (U)
					res.df[i] = -val;
				else
					res.df[i] = fabs(val);
				break;
			case 0x18:
				if (U)
					res.df[i] = round(val);
				else
					res.df[i] = rint(val);
				break;
			case 0x19:
				if (U)
					res.df[i] = trunc(val);
				else
					res.df[i] = floor(val);
				break;
			case 0x1A:
				if (U)
					res.d[i] = (uint64_t)rint(val);
				else
					res.d[i] = (uint64_t)(int64_t)
					    rint(val);
				break;
			case 0x1B:
				if (U)
					res.d[i] = (val < 0.0) ? 0 :
					    (uint64_t)floor(val);
				else
					res.d[i] = (uint64_t)(int64_t)
					    floor(val);
				break;
			case 0x1C:
				if (U) {
					double rv = round(val);
					res.d[i] = (rv < 0.0) ? 0 :
					    (uint64_t)rv;
				} else
					res.d[i] = (uint64_t)(int64_t)
					    round(val);
				break;
			case 0x1D:
				if (U)
					res.df[i] = (double)cpu->v[rn].d[i];
				else
					res.df[i] = (double)
					    (int64_t)cpu->v[rn].d[i];
				break;
			case 0x1E:
				if (U) {
					double rv = ceil(val);
					res.d[i] = (rv < 0.0) ? 0 :
					    (uint64_t)rv;
				} else
					res.d[i] = (uint64_t)(int64_t)
					    ceil(val);
				break;
			case 0x1F:
				if (U)
					res.df[i] = sqrt(val);
				else
					res.df[i] = 1.0 / val;
				break;
			default:
				LOG_WARN("unimpl fp 2reg misc opc=0x%x U=%u",
				    opcode, U);
				return EMU_UNIMPL;
			}
		}
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- FCVTN/FCVTL (FP narrow/lengthen) ---- */

static int
exec_fcvtn_fcvtl(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, sz, rn, rd;
	int		i;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	sz = bit(insn, 22);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	memset(&res, 0, sizeof(res));

	if (!U) {
		/* FCVTN/FCVTN2: narrow */
		if (sz == 0) {
			/* Double->Single: 2 doubles -> 2 singles */
			int base = Q ? 2 : 0;
			if (Q) res = cpu->v[rd];
			for (i = 0; i < 2; i++)
				res.sf[base + i] =
				    (float)cpu->v[rn].df[i];
		}
		/* else half precision, not commonly needed */
	} else {
		/* FCVTL/FCVTL2: lengthen */
		if (sz == 0) {
			/* Single->Double: 2 singles -> 2 doubles */
			int base = Q ? 2 : 0;
			for (i = 0; i < 2; i++)
				res.df[i] =
				    (double)cpu->v[rn].sf[base + i];
		}
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Across-lanes ---- */

static int
exec_across_lanes(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, size, opcode, rn, rd;
	int		i, elems;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	size = bits(insn, 23, 22);
	opcode = bits(insn, 16, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	elems = velem_count(size, Q);
	memset(&cpu->v[rd], 0, sizeof(vreg_t));

	switch (opcode) {
	case 0x03:	/* SADDLV / UADDLV */
	{
		int dsize = size + 1;
		int64_t sum_s = 0;
		uint64_t sum_u = 0;
		for (i = 0; i < elems; i++) {
			if (U)
				sum_u += velem_u(&cpu->v[rn], size, i);
			else
				sum_s += velem_s(&cpu->v[rn], size, i);
		}
		if (U)
			velem_set(&cpu->v[rd], dsize, 0, sum_u);
		else
			velem_set(&cpu->v[rd], dsize, 0, (uint64_t)sum_s);
		break;
	}

	case 0x0A:	/* SMAXV / UMAXV */
		if (U) {
			uint64_t max = velem_u(&cpu->v[rn], size, 0);
			for (i = 1; i < elems; i++) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				if (v > max) max = v;
			}
			velem_set(&cpu->v[rd], size, 0, max);
		} else {
			int64_t max = velem_s(&cpu->v[rn], size, 0);
			for (i = 1; i < elems; i++) {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				if (v > max) max = v;
			}
			velem_set(&cpu->v[rd], size, 0, (uint64_t)max);
		}
		break;

	case 0x1A:	/* SMINV / UMINV */
		if (U) {
			uint64_t min = velem_u(&cpu->v[rn], size, 0);
			for (i = 1; i < elems; i++) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				if (v < min) min = v;
			}
			velem_set(&cpu->v[rd], size, 0, min);
		} else {
			int64_t min = velem_s(&cpu->v[rn], size, 0);
			for (i = 1; i < elems; i++) {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				if (v < min) min = v;
			}
			velem_set(&cpu->v[rd], size, 0, (uint64_t)min);
		}
		break;

	case 0x1B:	/* ADDV */
	{
		uint64_t sum = 0;
		for (i = 0; i < elems; i++)
			sum += velem_u(&cpu->v[rn], size, i);
		velem_set(&cpu->v[rd], size, 0, sum);
		break;
	}

	default:
		LOG_WARN("unimplemented across-lanes opcode=0x%02x U=%u "
		    "at 0x%llx", opcode, U,
		    (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	return EMU_OK;
}

/* ---- Shift by immediate ---- */

static int
exec_shift_imm(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, U, immh, immb, opcode, rn, rd;
	int		i, shift, elems, size, esize_b;
	vreg_t		res;

	Q = bit(insn, 30);
	U = bit(insn, 29);
	immh = bits(insn, 22, 19);
	immb = bits(insn, 18, 16);
	opcode = bits(insn, 15, 11);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	/* Determine element size from immh */
	if (immh & 8)		{ size = 3; esize_b = 64; }
	else if (immh & 4)	{ size = 2; esize_b = 32; }
	else if (immh & 2)	{ size = 1; esize_b = 16; }
	else if (immh & 1)	{ size = 0; esize_b = 8; }
	else			return EMU_UNIMPL;

	shift = (int)((immh << 3) | immb);
	elems = velem_count(size, Q);
	memset(&res, 0, sizeof(res));

	switch (opcode) {
	case 0x00:	/* SSHR / USHR */
	{
		int sh = (2 * esize_b) - shift;
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				velem_set(&res, size, i, v >> sh);
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				velem_set(&res, size, i,
				    (uint64_t)(v >> sh));
			}
		}
		break;
	}

	case 0x01:	/* SSRA / USRA */
	{
		int sh = (2 * esize_b) - shift;
		for (i = 0; i < elems; i++) {
			uint64_t acc = velem_u(&cpu->v[rd], size, i);
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				velem_set(&res, size, i, acc + (v >> sh));
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				velem_set(&res, size, i,
				    acc + (uint64_t)(v >> sh));
			}
		}
		break;
	}

	case 0x02:	/* SRSHR / URSHR */
	{
		int sh = (2 * esize_b) - shift;
		for (i = 0; i < elems; i++) {
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				uint64_t round = (sh > 0) ?
				    (v >> (sh - 1)) & 1 : 0;
				velem_set(&res, size, i,
				    (v >> sh) + round);
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				int64_t round = (sh > 0) ?
				    (v >> (sh - 1)) & 1 : 0;
				velem_set(&res, size, i,
				    (uint64_t)((v >> sh) + round));
			}
		}
		break;
	}

	case 0x03:	/* SRSRA / URSRA */
	{
		int sh = (2 * esize_b) - shift;
		for (i = 0; i < elems; i++) {
			uint64_t acc = velem_u(&cpu->v[rd], size, i);
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				uint64_t round = (sh > 0) ?
				    (v >> (sh - 1)) & 1 : 0;
				velem_set(&res, size, i,
				    acc + (v >> sh) + round);
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				int64_t round = (sh > 0) ?
				    (v >> (sh - 1)) & 1 : 0;
				velem_set(&res, size, i,
				    acc + (uint64_t)((v >> sh) + round));
			}
		}
		break;
	}

	case 0x05:	/* SHL (U=0) / SLI (U=1) */
	{
		int sh = shift - esize_b;
		for (i = 0; i < elems; i++) {
			uint64_t v = velem_u(&cpu->v[rn], size, i);
			if (U) {
				/* SLI: shift left and insert */
				uint64_t mask = esize_mask(size);
				uint64_t shifted = (v << sh) & mask;
				uint64_t keep_mask =
				    ((uint64_t)1 << sh) - 1;
				uint64_t old = velem_u(&cpu->v[rd],
				    size, i);
				velem_set(&res, size, i,
				    (shifted & ~keep_mask) |
				    (old & keep_mask));
			} else {
				/* SHL */
				velem_set(&res, size, i, v << sh);
			}
		}
		break;
	}

	case 0x07:	/* SQSHLU (U=1) / SRI (U=0 - shift right and insert) */
		if (U) {
			/* SQSHLU */
			int sh = shift - esize_b;
			for (i = 0; i < elems; i++) {
				int64_t v = velem_s(&cpu->v[rn], size, i);
				int64_t r = v << sh;
				velem_set(&res, size, i,
				    sat_unsigned(r, esize_b));
			}
		} else {
			/* SRI */
			int sh = (2 * esize_b) - shift;
			uint64_t mask = esize_mask(size);
			for (i = 0; i < elems; i++) {
				uint64_t v = velem_u(&cpu->v[rn], size, i);
				uint64_t shifted = v >> sh;
				uint64_t keep_bits = esize_b - sh;
				uint64_t keep_mask = mask &
				    ~(((uint64_t)1 << keep_bits) - 1);
				uint64_t old = velem_u(&cpu->v[rd],
				    size, i);
				velem_set(&res, size, i,
				    (old & keep_mask) | shifted);
			}
		}
		break;

	case 0x0A:	/* SHL (long): SSHLL/USHLL */
	{
		int sh = shift - esize_b;
		int part = Q;
		int dsize = size + 1;
		int src_elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;

		for (i = 0; i < src_elems; i++) {
			int si = part * src_elems + i;
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, si);
				velem_set(&res, dsize, i, v << sh);
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, si);
				velem_set(&res, dsize, i,
				    (uint64_t)(v << sh));
			}
		}
		break;
	}

	case 0x08:	/* SHRN/RSHRN (U=0/1) */
	{
		int sh = (2 * esize_b) - shift;
		int part = Q;
		int src_size = size + 1;
		int src_elems = (src_size == 1) ? 8 :
		    (src_size == 2) ? 4 : 2;

		if (part) res = cpu->v[rd];

		for (i = 0; i < src_elems; i++) {
			uint64_t v = velem_u(&cpu->v[rn], src_size, i);
			if (U) {
				/* RSHRN: rounding */
				uint64_t round = (sh > 0) ?
				    (v >> (sh - 1)) & 1 : 0;
				v = (v >> sh) + round;
			} else {
				v = v >> sh;
			}
			velem_set(&res, size, part * src_elems + i, v);
		}
		break;
	}

	case 0x09:	/* SQSHRN/UQSHRN (U=0/1 for signed/unsigned) + SQRSHRN/UQRSHRN with rounding */
	{
		int sh = (2 * esize_b) - shift;
		int part = Q;
		int src_size = size + 1;
		int bw = esize_b;
		int src_elems = (src_size == 1) ? 8 :
		    (src_size == 2) ? 4 : 2;

		if (part) res = cpu->v[rd];

		for (i = 0; i < src_elems; i++) {
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn],
				    src_size, i);
				v = v >> sh;
				velem_set(&res, size,
				    part * src_elems + i,
				    usat_unsigned(v, bw));
			} else {
				int64_t v = velem_s(&cpu->v[rn],
				    src_size, i);
				v = v >> sh;
				velem_set(&res, size,
				    part * src_elems + i,
				    (uint64_t)sat_signed(v, bw));
			}
		}
		break;
	}

	case 0x14:	/* SSHLL/USHLL - alias mapped here for some encodings */
	{
		int sh = shift - esize_b;
		int part = Q;
		int dsize = size + 1;
		int src_elems = (dsize == 1) ? 8 : (dsize == 2) ? 4 : 2;

		for (i = 0; i < src_elems; i++) {
			int si = part * src_elems + i;
			if (U) {
				uint64_t v = velem_u(&cpu->v[rn], size, si);
				velem_set(&res, dsize, i, v << sh);
			} else {
				int64_t v = velem_s(&cpu->v[rn], size, si);
				velem_set(&res, dsize, i,
				    (uint64_t)(v << sh));
			}
		}
		break;
	}

	default:
		LOG_WARN("unimplemented shift-imm opcode=0x%02x U=%u "
		    "at 0x%llx", opcode, U,
		    (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- Permute: ZIP/UZP/TRN ---- */

static int
exec_permute(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, size, rm, opcode, rn, rd;
	int		i, elems, half;
	vreg_t		tmp;

	Q = bit(insn, 30);
	size = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	opcode = bits(insn, 14, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	elems = velem_count(size, Q);
	half = elems / 2;
	memset(&tmp, 0, sizeof(tmp));

	switch (opcode) {
	case 0x01:	/* UZP1 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, i,
			    velem_u(&cpu->v[rn], size, 2 * i));
			velem_set(&tmp, size, half + i,
			    velem_u(&cpu->v[rm], size, 2 * i));
		}
		break;

	case 0x02:	/* TRN1 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, 2 * i,
			    velem_u(&cpu->v[rn], size, 2 * i));
			velem_set(&tmp, size, 2 * i + 1,
			    velem_u(&cpu->v[rm], size, 2 * i));
		}
		break;

	case 0x03:	/* ZIP1 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, 2 * i,
			    velem_u(&cpu->v[rn], size, i));
			velem_set(&tmp, size, 2 * i + 1,
			    velem_u(&cpu->v[rm], size, i));
		}
		break;

	case 0x05:	/* UZP2 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, i,
			    velem_u(&cpu->v[rn], size, 2 * i + 1));
			velem_set(&tmp, size, half + i,
			    velem_u(&cpu->v[rm], size, 2 * i + 1));
		}
		break;

	case 0x06:	/* TRN2 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, 2 * i,
			    velem_u(&cpu->v[rn], size, 2 * i + 1));
			velem_set(&tmp, size, 2 * i + 1,
			    velem_u(&cpu->v[rm], size, 2 * i + 1));
		}
		break;

	case 0x07:	/* ZIP2 */
		for (i = 0; i < half; i++) {
			velem_set(&tmp, size, 2 * i,
			    velem_u(&cpu->v[rn], size, half + i));
			velem_set(&tmp, size, 2 * i + 1,
			    velem_u(&cpu->v[rm], size, half + i));
		}
		break;

	default:
		LOG_WARN("unimplemented permute opcode=%u at 0x%llx",
		    opcode, (unsigned long long)cpu->pc);
		return EMU_UNIMPL;
	}

	cpu->v[rd] = tmp;
	return EMU_OK;
}

/* ---- TBL/TBX ---- */

static int
exec_tbl_tbx(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, rm, len, op, rn, rd;
	int		bytes, i, nregs;
	uint8_t		table[64];
	int		tbl_size;
	vreg_t		res;

	Q = bit(insn, 30);
	rm = bits(insn, 20, 16);
	len = bits(insn, 14, 13);
	op = bit(insn, 12);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	nregs = (int)len + 1;
	tbl_size = nregs * 16;
	bytes = Q ? 16 : 8;

	for (i = 0; i < nregs; i++)
		memcpy(table + i * 16, &cpu->v[(rn + i) & 31], 16);

	if (op) {
		/* TBX: keep destination value for out-of-range indices */
		res = cpu->v[rd];
	} else {
		/* TBL: zero for out-of-range indices */
		memset(&res, 0, sizeof(res));
	}

	for (i = 0; i < bytes; i++) {
		uint8_t idx = cpu->v[rm].b[i];
		if (idx < tbl_size)
			res.b[i] = table[idx];
		/* else: TBX keeps old, TBL keeps zero */
	}

	cpu->v[rd] = res;
	return EMU_OK;
}

/* ---- EXT: extract from pair of vectors ---- */

static int
exec_ext(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	Q, rm, imm4, rn, rd;
	int		bytes, i;
	uint8_t		combined[32];

	Q = bit(insn, 30);
	rm = bits(insn, 20, 16);
	imm4 = bits(insn, 14, 11);
	rn = bits(insn, 9, 5);
	rd = bits(insn, 4, 0);

	bytes = Q ? 16 : 8;

	memcpy(combined, &cpu->v[rn], (size_t)bytes);
	memcpy(combined + bytes, &cpu->v[rm], (size_t)bytes);

	memset(&cpu->v[rd], 0, sizeof(vreg_t));
	for (i = 0; i < bytes; i++)
		cpu->v[rd].b[i] = combined[imm4 + i];

	return EMU_OK;
}

/* ---- FRECPE/FRSQRTE (two-reg misc FP reciprocal estimates) ---- */
/* These are dispatched via exec_two_reg_misc_fp opcode 0x1F */

/* ---- Main dispatch function ---- */

int
exec_simd(cpu_state_t *cpu, uint32_t insn)
{
	/*
	 * Scalar FP: bits[28:24] = 11110
	 */
	if (bits(insn, 28, 24) == 0x1E) {
		/* FP data processing 1-source */
		if (bits(insn, 14, 10) == 0x10 && bit(insn, 21) == 1 &&
		    bits(insn, 11, 10) == 0)
			return exec_fp_data1(cpu, insn);

		/* FP compare */
		if (bits(insn, 13, 10) == 0x08 && bits(insn, 4, 2) == 0)
			return exec_fp_compare(cpu, insn);

		/* FP data processing 2-source */
		if (bit(insn, 11) == 1 && bit(insn, 10) == 0 &&
		    bit(insn, 21) == 1 && bits(insn, 14, 12) != 4)
			return exec_fp_data2(cpu, insn);

		/* FP conditional select */
		if (bits(insn, 11, 10) == 3 && bit(insn, 21) == 1)
			return exec_fp_csel(cpu, insn);

		/* FP <-> integer conversion */
		if (bit(insn, 21) == 1 && bits(insn, 14, 10) == 0x00)
			return exec_fp_int_conv(cpu, insn);
	}

	/* FP data processing 3-source */
	if (bits(insn, 28, 24) == 0x1F)
		return exec_fp_data3(cpu, insn);

	/*
	 * Advanced SIMD data processing.
	 * bits[28:24] = 0x0E or 0x0F
	 */
	uint32_t op0 = bits(insn, 28, 24);

	if (op0 != 0x0E && op0 != 0x0F) {
		LOG_WARN("unimplemented SIMD/FP at 0x%llx: 0x%08x",
		    (unsigned long long)cpu->pc, insn);
		return EMU_UNIMPL;
	}

	/* ---- Copy operations (DUP, INS, UMOV) ---- */

	/* DUP (general): 0 Q 0 01110000 imm5 0 00011 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x03)
		return exec_dup_general(cpu, insn);

	/* UMOV: 0 Q 0 01110000 imm5 0 01111 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x0F) {
		uint32_t imm5 = bits(insn, 20, 16);
		uint32_t rn = bits(insn, 9, 5);
		uint32_t rd = bits(insn, 4, 0);
		uint64_t val = 0;

		if (imm5 & 1)
			val = cpu->v[rn].b[(imm5 >> 1) & 0xF];
		else if (imm5 & 2)
			val = cpu->v[rn].h[(imm5 >> 2) & 0x7];
		else if (imm5 & 4)
			val = cpu->v[rn].s[(imm5 >> 3) & 0x3];
		else if (imm5 & 8)
			val = cpu->v[rn].d[(imm5 >> 4) & 0x1];
		cpu_set_xreg(cpu, rd, val);
		return EMU_OK;
	}

	/* SMOV: 0 Q 0 01110000 imm5 0 00101 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x05) {
		uint32_t imm5 = bits(insn, 20, 16);
		uint32_t rn = bits(insn, 9, 5);
		uint32_t rd = bits(insn, 4, 0);
		int64_t val = 0;

		if (imm5 & 1)
			val = (int8_t)cpu->v[rn].b[(imm5 >> 1) & 0xF];
		else if (imm5 & 2)
			val = (int16_t)cpu->v[rn].h[(imm5 >> 2) & 0x7];
		else if (imm5 & 4)
			val = (int32_t)cpu->v[rn].s[(imm5 >> 3) & 0x3];
		cpu_set_xreg(cpu, rd, (uint64_t)val);
		return EMU_OK;
	}

	/* INS (general): 0 1 0 01110000 imm5 0 00111 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x07 &&
	    bit(insn, 30) == 1) {
		uint32_t imm5 = bits(insn, 20, 16);
		uint32_t rn = bits(insn, 9, 5);
		uint32_t rd = bits(insn, 4, 0);
		uint64_t val = cpu_xreg(cpu, rn);

		if (imm5 & 1)
			cpu->v[rd].b[(imm5 >> 1) & 0xF] = (uint8_t)val;
		else if (imm5 & 2)
			cpu->v[rd].h[(imm5 >> 2) & 0x7] = (uint16_t)val;
		else if (imm5 & 4)
			cpu->v[rd].s[(imm5 >> 3) & 0x3] = (uint32_t)val;
		else if (imm5 & 8)
			cpu->v[rd].d[(imm5 >> 4) & 0x1] = val;
		return EMU_OK;
	}

	/* DUP (element): 0 Q 0 01110000 imm5 0 00001 Rn Rd */
	if (bits(insn, 29, 21) == 0x070 && bits(insn, 15, 10) == 0x01)
		return exec_dup_element(cpu, insn);

	/* INS (element): 0 1 1 01110000 imm5 0 imm4 1 Rn Rd */
	if (bit(insn, 31) == 0 && bit(insn, 29) == 1 &&
	    bits(insn, 28, 21) == 0x70 && bit(insn, 10) == 1 &&
	    bit(insn, 15) == 0)
		return exec_ins_element(cpu, insn);

	/* ---- Modified immediate: MOVI/MVNI/ORR imm/BIC imm ---- */
	/* 0 Q op 0111100000 abc cmode o2 1 defgh Rd */
	if (bits(insn, 28, 19) == 0x1E0 && bits(insn, 11, 10) == 0x01)
		return exec_simd_mod_imm(cpu, insn);

	/* ---- Shift by immediate: bits[28:24] = 0x0F ---- */
	if (op0 == 0x0F && bits(insn, 11, 10) == 0x01 &&
	    bits(insn, 22, 19) != 0) {
		/* Dispatch based on opcode */
		uint32_t opc = bits(insn, 15, 11);

		/* Filter: only shift-imm opcodes, not SSHLL which has
		 * specific format */
		if (opc <= 0x14)
			return exec_shift_imm(cpu, insn);
	}

	/* ---- Load/Store multiple structures ---- */
	/* 0 Q 001100 L 0 Rm opcode size Rn Rt (no post-index) */
	/* 0 Q 001100 L 1 Rm opcode size Rn Rt (post-index) */
	if (bit(insn, 31) == 0 && bits(insn, 29, 24) == 0x0C &&
	    bit(insn, 21) == 0)
		return exec_simd_ldst_multi(cpu, insn);

	/* ---- LD1R: 0 Q 001101 L 1 Rm 110 0 size Rn Rt ---- */
	if (bit(insn, 31) == 0 && bits(insn, 29, 24) == 0x0D &&
	    bit(insn, 22) == 1 && bits(insn, 15, 13) == 0x06 &&
	    bit(insn, 12) == 0)
		return exec_simd_ld1r(cpu, insn);

	/* ---- LD single structure (to single lane) ---- */
	/* 0 Q 001101 L 0/1 Rm opcode S size Rn Rt */
	if (bit(insn, 31) == 0 && bits(insn, 29, 24) == 0x0D &&
	    bit(insn, 21) == 0) {
		/* Simple LD1/ST1 single structure to lane */
		uint32_t Q2 = bit(insn, 30);
		uint32_t L = bit(insn, 22);
		uint32_t opc = bits(insn, 15, 13);
		uint32_t S = bit(insn, 12);
		uint32_t sz = bits(insn, 11, 10);
		uint32_t rn2 = bits(insn, 9, 5);
		uint32_t rt2 = bits(insn, 4, 0);
		uint64_t addr = cpu_xreg_sp(cpu, rn2);
		int post = bit(insn, 23);
		uint32_t rm2 = bits(insn, 20, 16);

		if (opc == 0x00) {
			/* Byte */
			int idx = (Q2 << 3) | (S << 2) | sz;
			if (L) {
				uint8_t val;
				if (mem_read8(cpu->mem, addr, &val) != 0)
					return EMU_SEGFAULT;
				cpu->v[rt2].b[idx] = val;
			} else {
				if (mem_write8(cpu->mem, addr,
				    cpu->v[rt2].b[idx]) != 0)
					return EMU_SEGFAULT;
			}
			if (post) {
				uint64_t off = (rm2 == 31) ? 1 :
				    cpu_xreg(cpu, rm2);
				cpu_set_xreg_sp(cpu, rn2, addr + off);
			}
			return EMU_OK;
		}
		if (opc == 0x02) {
			/* Halfword */
			int idx = (Q2 << 2) | (S << 1) | (sz >> 1);
			if (L) {
				uint16_t val;
				if (mem_read16(cpu->mem, addr, &val) != 0)
					return EMU_SEGFAULT;
				cpu->v[rt2].h[idx] = val;
			} else {
				if (mem_write16(cpu->mem, addr,
				    cpu->v[rt2].h[idx]) != 0)
					return EMU_SEGFAULT;
			}
			if (post) {
				uint64_t off = (rm2 == 31) ? 2 :
				    cpu_xreg(cpu, rm2);
				cpu_set_xreg_sp(cpu, rn2, addr + off);
			}
			return EMU_OK;
		}
		if (opc == 0x04) {
			if ((sz & 1) == 0) {
				/* Word */
				int idx = (Q2 << 1) | S;
				if (L) {
					uint32_t val;
					if (mem_read32(cpu->mem, addr,
					    &val) != 0)
						return EMU_SEGFAULT;
					cpu->v[rt2].s[idx] = val;
				} else {
					if (mem_write32(cpu->mem, addr,
					    cpu->v[rt2].s[idx]) != 0)
						return EMU_SEGFAULT;
				}
				if (post) {
					uint64_t off = (rm2 == 31) ? 4 :
					    cpu_xreg(cpu, rm2);
					cpu_set_xreg_sp(cpu, rn2,
					    addr + off);
				}
				return EMU_OK;
			} else {
				/* Doubleword */
				int idx = Q2;
				if (L) {
					uint64_t val;
					if (mem_read64(cpu->mem, addr,
					    &val) != 0)
						return EMU_SEGFAULT;
					cpu->v[rt2].d[idx] = val;
				} else {
					if (mem_write64(cpu->mem, addr,
					    cpu->v[rt2].d[idx]) != 0)
						return EMU_SEGFAULT;
				}
				if (post) {
					uint64_t off = (rm2 == 31) ? 8 :
					    cpu_xreg(cpu, rm2);
					cpu_set_xreg_sp(cpu, rn2,
					    addr + off);
				}
				return EMU_OK;
			}
		}
	}

	/* ---- Advanced SIMD three-same ---- */
	/* 0 Q U 01110 size 1 Rm opcode 1 Rn Rd */
	if (op0 == 0x0E && bit(insn, 21) == 1 && bit(insn, 10) == 1) {
		uint32_t sz = bits(insn, 23, 22);
		uint32_t opc = bits(insn, 15, 11);

		/* FP three-same: bit[23]=0, bit[10]=1, opcodes vary */
		/* Detect FP: the encoding uses size[1] (bit23) = 0 for
		 * FP operations when opcode >= 0x03 and specific patterns */
		/* FP three-same is: 0 Q U 01110 a 0/1 1 Rm opcode 1 Rn Rd
		 * where bit[23]=0 always for FP, and opcodes start at 0x18
		 * OR specific opcodes like 0x03-0x07 with bit23=0 */

		/* Heuristic: if opcode maps to a known FP op with sz=0b0x
		 * pattern */
		int is_fp = 0;

		/* Standard FP three-same opcodes */
		if (opc >= 0x18 && opc <= 0x1F)
			is_fp = 1;
		/* Also opcodes 0x03-0x07 can be FP */
		if (opc >= 0x03 && opc <= 0x07 && (sz & 2) == 0)
			is_fp = 1;
		/* 0x0C-0x0F with sz bit pattern */
		if (opc >= 0x0C && opc <= 0x0F && (sz & 2) == 0)
			is_fp = 1;

		if (is_fp)
			return exec_three_same_fp(cpu, insn);

		return exec_three_same(cpu, insn);
	}

	/* ---- Advanced SIMD three-different ---- */
	/* 0 Q U 01110 size 1 Rm opcode 00 Rn Rd */
	if (op0 == 0x0E && bit(insn, 21) == 1 && bits(insn, 10, 10) == 0 &&
	    bit(insn, 11) == 0)
		return exec_three_diff(cpu, insn);

	/* ---- Advanced SIMD two-reg misc ---- */
	/* 0 Q U 01110 size 10000 opcode 10 Rn Rd */
	if (op0 == 0x0E && bits(insn, 21, 17) == 0x10 &&
	    bits(insn, 11, 10) == 0x02) {
		uint32_t opc = bits(insn, 16, 12);
		uint32_t sz = bits(insn, 23, 22);

		/* FP two-reg misc: size encodes FP type */
		/* FP opcodes: 0x0C-0x0F, 0x18-0x1F with sz=0b0x or 0b1x */
		if ((opc >= 0x0C && opc <= 0x0F) ||
		    (opc >= 0x18 && opc <= 0x1F)) {
			/* Check if truly FP (sz field is 0b0x for float) */
			if ((sz & 2) == 0)
				return exec_two_reg_misc_fp(cpu, insn);
		}

		/* FCVTN/FCVTL: opcode=0x16 or 0x17 */
		if (opc == 0x16 || opc == 0x17) {
			if ((sz & 2) == 0)
				return exec_fcvtn_fcvtl(cpu, insn);
		}

		return exec_two_reg_misc(cpu, insn);
	}

	/* ---- Advanced SIMD across lanes ---- */
	/* 0 Q U 01110 size 11000 opcode 10 Rn Rd */
	if (op0 == 0x0E && bits(insn, 21, 17) == 0x18 &&
	    bits(insn, 11, 10) == 0x02)
		return exec_across_lanes(cpu, insn);

	/* ---- Permute ---- */
	/* 0 Q 0 01110 size 0 Rm 0 opcode 10 Rn Rd */
	if (op0 == 0x0E && bit(insn, 21) == 0 && bit(insn, 15) == 0 &&
	    bit(insn, 10) == 0 && bit(insn, 11) == 1) {
		uint32_t opc = bits(insn, 14, 12);
		if (opc != 0 && opc != 4)
			return exec_permute(cpu, insn);
	}

	/* ---- TBL/TBX ---- */
	/* 0 Q 0 01110 00 0 Rm 0 len op 000 Rn Rd */
	if (op0 == 0x0E && bits(insn, 23, 22) == 0x00 &&
	    bit(insn, 21) == 0 && bit(insn, 15) == 0 &&
	    bits(insn, 11, 10) == 0x00)
		return exec_tbl_tbx(cpu, insn);

	/* ---- EXT ---- */
	/* 0 Q 10 1110 00 0 Rm 0 imm4 0 Rn Rd */
	if (op0 == 0x0E && bit(insn, 29) == 1 &&
	    bits(insn, 23, 22) == 0x00 && bit(insn, 21) == 0 &&
	    bit(insn, 15) == 0 && bit(insn, 10) == 0)
		return exec_ext(cpu, insn);

	LOG_WARN("unimplemented SIMD/FP at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
