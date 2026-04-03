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

static int
do_load(cpu_state_t *cpu, uint64_t addr, int size, int opc, int rt, int is_vec)
{
	if (is_vec) {
		/*
		 * SIMD/FP loads.
		 * size=0,opc=01: LDR B (8-bit)
		 * size=1,opc=01: LDR H (16-bit)
		 * size=2,opc=01: LDR S (32-bit)
		 * size=3,opc=01: LDR D (64-bit)
		 * size=0,opc=11: LDR Q (128-bit)
		 */
		memset(&cpu->v[rt], 0, sizeof(vreg_t));

		if (size == 0 && opc == 1) {
			return mem_read8(cpu->mem, addr, &cpu->v[rt].b[0]);
		} else if (size == 1 && opc == 1) {
			return mem_read16(cpu->mem, addr, &cpu->v[rt].h[0]);
		} else if (size == 2 && opc == 1) {
			return mem_read32(cpu->mem, addr, &cpu->v[rt].s[0]);
		} else if (size == 3 && opc == 1) {
			return mem_read64(cpu->mem, addr, &cpu->v[rt].d[0]);
		} else if (size == 0 && opc == 3) {
			/* 128-bit load */
			if (mem_read64(cpu->mem, addr, &cpu->v[rt].d[0]) != 0)
				return -1;
			return mem_read64(cpu->mem, addr + 8,
			    &cpu->v[rt].d[1]);
		}
		LOG_WARN("unimplemented SIMD load size=%d opc=%d at 0x%llx",
		    size, opc, (unsigned long long)cpu->pc);
		return -1;
	}

	/* GPR loads */
	switch ((size << 2) | opc) {
	case 0x01: {	/* LDRB (8-bit zero-extend) */
		uint8_t val;
		if (mem_read8(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, (uint64_t)val);
		return 0;
	}
	case 0x02: {	/* LDRSB (64-bit sign-extend) */
		uint8_t val;
		if (mem_read8(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, (uint64_t)sign_extend(val, 8));
		return 0;
	}
	case 0x03: {	/* LDRSB (32-bit sign-extend) */
		uint8_t val;
		if (mem_read8(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_wreg(cpu, rt, (uint32_t)sign_extend(val, 8));
		return 0;
	}
	case 0x05: {	/* LDRH (16-bit zero-extend) */
		uint16_t val;
		if (mem_read16(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, (uint64_t)val);
		return 0;
	}
	case 0x06: {	/* LDRSH (64-bit sign-extend) */
		uint16_t val;
		if (mem_read16(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, (uint64_t)sign_extend(val, 16));
		return 0;
	}
	case 0x07: {	/* LDRSH (32-bit sign-extend) */
		uint16_t val;
		if (mem_read16(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_wreg(cpu, rt, (uint32_t)sign_extend(val, 16));
		return 0;
	}
	case 0x09: {	/* LDR (32-bit) */
		uint32_t val;
		if (mem_read32(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_wreg(cpu, rt, val);
		return 0;
	}
	case 0x0A: {	/* LDRSW (64-bit sign-extend from 32) */
		uint32_t val;
		if (mem_read32(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, (uint64_t)sign_extend(val, 32));
		return 0;
	}
	case 0x0D: {	/* LDR (64-bit) */
		uint64_t val;
		if (mem_read64(cpu->mem, addr, &val) != 0)
			return -1;
		cpu_set_xreg(cpu, rt, val);
		return 0;
	}
	default:
		LOG_WARN("unimplemented GPR load size=%d opc=%d at 0x%llx",
		    size, opc, (unsigned long long)cpu->pc);
		return -1;
	}
}

static int
do_store(cpu_state_t *cpu, uint64_t addr, int size, int opc, int rt,
    int is_vec)
{
	if (is_vec) {
		if (size == 0 && opc == 0) {
			return mem_write8(cpu->mem, addr, cpu->v[rt].b[0]);
		} else if (size == 1 && opc == 0) {
			return mem_write16(cpu->mem, addr, cpu->v[rt].h[0]);
		} else if (size == 2 && opc == 0) {
			return mem_write32(cpu->mem, addr, cpu->v[rt].s[0]);
		} else if (size == 3 && opc == 0) {
			return mem_write64(cpu->mem, addr, cpu->v[rt].d[0]);
		} else if (size == 0 && opc == 2) {
			/* 128-bit store */
			if (mem_write64(cpu->mem, addr,
			    cpu->v[rt].d[0]) != 0)
				return -1;
			return mem_write64(cpu->mem, addr + 8,
			    cpu->v[rt].d[1]);
		}
		LOG_WARN("unimplemented SIMD store size=%d opc=%d at 0x%llx",
		    size, opc, (unsigned long long)cpu->pc);
		return -1;
	}

	/* GPR stores - opc must be 0 for stores */
	switch (size) {
	case 0:	/* STRB */
		return mem_write8(cpu->mem, addr,
		    (uint8_t)cpu_xreg(cpu, rt));
	case 1:	/* STRH */
		return mem_write16(cpu->mem, addr,
		    (uint16_t)cpu_xreg(cpu, rt));
	case 2:	/* STR (32-bit) */
		return mem_write32(cpu->mem, addr, cpu_wreg(cpu, rt));
	case 3:	/* STR (64-bit) */
		return mem_write64(cpu->mem, addr, cpu_xreg(cpu, rt));
	default:
		return -1;
	}
}

static int
exec_ldst_unsigned_offset(cpu_state_t *cpu, uint32_t insn)
{
	int		size, V, opc;
	uint32_t	imm12, rn, rt;
	uint64_t	offset, addr;
	int		is_load;

	size = bits(insn, 31, 30);
	V = bit(insn, 26);
	opc = bits(insn, 23, 22);
	imm12 = bits(insn, 21, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);

	/* Scale offset by access size */
	if (V) {
		int scale;
		if (opc == 0 || opc == 1)
			scale = size;
		else if (size == 0 && opc == 3)
			scale = 4;	/* 128-bit */
		else
			scale = size;
		offset = (uint64_t)imm12 << scale;
	} else {
		offset = (uint64_t)imm12 << size;
	}

	addr = cpu_xreg_sp(cpu, rn) + offset;

	is_load = (opc & 1) || (opc == 2);
	if (V)
		is_load = (opc == 1 || opc == 3);

	if (is_load) {
		if (do_load(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	} else {
		if (do_store(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	}

	return EMU_OK;
}

static int
exec_ldst_imm9(cpu_state_t *cpu, uint32_t insn)
{
	int		size, V, opc, idx_type;
	uint32_t	rn, rt;
	int64_t		imm9;
	uint64_t	addr;
	int		is_load;

	size = bits(insn, 31, 30);
	V = bit(insn, 26);
	opc = bits(insn, 23, 22);
	imm9 = sign_extend(bits(insn, 20, 12), 9);
	idx_type = bits(insn, 11, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);

	addr = cpu_xreg_sp(cpu, rn);

	if (idx_type == 0) {
		/* Unscaled immediate (LDUR/STUR) */
		addr = (uint64_t)((int64_t)addr + imm9);
	} else if (idx_type == 1) {
		/* Post-index: use base, then update */
		/* addr stays as base for access */
	} else if (idx_type == 3) {
		/* Pre-index: update first, then use */
		addr = (uint64_t)((int64_t)addr + imm9);
	} else {
		return EMU_ERR;
	}

	is_load = (opc & 1) || (opc == 2);
	if (V)
		is_load = (opc == 1 || opc == 3);

	if (is_load) {
		if (do_load(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	} else {
		if (do_store(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	}

	/* Writeback for pre/post-index */
	if (idx_type == 1) {
		/* Post-index: update base after access */
		cpu_set_xreg_sp(cpu, rn,
		    (uint64_t)((int64_t)cpu_xreg_sp(cpu, rn) + imm9));
	} else if (idx_type == 3) {
		/* Pre-index: base already updated, write it back */
		cpu_set_xreg_sp(cpu, rn, addr);
	}

	return EMU_OK;
}

static int
exec_ldst_reg_offset(cpu_state_t *cpu, uint32_t insn)
{
	int		size, V, opc;
	uint32_t	rm, option, S, rn, rt;
	uint64_t	offset, addr;
	int		is_load, shift;

	size = bits(insn, 31, 30);
	V = bit(insn, 26);
	opc = bits(insn, 23, 22);
	rm = bits(insn, 20, 16);
	option = bits(insn, 15, 13);
	S = bits(insn, 12, 12);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);

	/* Determine shift amount */
	if (S) {
		if (V && (size == 0) && (opc & 2))
			shift = 4;	/* 128-bit */
		else
			shift = size;
	} else {
		shift = 0;
	}

	offset = extend_reg(cpu, (int)rm, (int)option, shift);
	addr = cpu_xreg_sp(cpu, rn) + offset;

	is_load = (opc & 1) || (opc == 2);
	if (V)
		is_load = (opc == 1 || opc == 3);

	if (is_load) {
		if (do_load(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	} else {
		if (do_store(cpu, addr, size, opc, (int)rt, V) != 0)
			return EMU_SEGFAULT;
	}

	return EMU_OK;
}

static int
exec_ldst_pair(cpu_state_t *cpu, uint32_t insn)
{
	int		opc, V, mode, L;
	uint32_t	rt2, rn, rt;
	int64_t		offset;
	uint64_t	addr;
	int		scale;

	opc = bits(insn, 31, 30);
	V = bit(insn, 26);
	mode = bits(insn, 24, 23);
	L = bit(insn, 22);
	rt2 = bits(insn, 14, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);

	if (mode == 0)
		return EMU_UNIMPL;	/* no-allocate hint, not needed */

	if (V) {
		scale = 2 + opc;	/* S=2(4B), D=3(8B), Q=4(16B) */
	} else {
		scale = (opc & 1) ? 3 : 2;	/* 32-bit=2, 64-bit=3 */
	}

	offset = sign_extend(bits(insn, 21, 15), 7) << scale;
	addr = cpu_xreg_sp(cpu, rn);

	/* Pre-index or signed offset: add offset before access */
	if (mode == 2 || mode == 3)
		addr = (uint64_t)((int64_t)addr + offset);

	if (V) {
		/* SIMD/FP pair */
		if (L) {
			/* Load pair */
			memset(&cpu->v[rt], 0, sizeof(vreg_t));
			memset(&cpu->v[rt2], 0, sizeof(vreg_t));
			switch (opc) {
			case 0:	/* 32-bit */
				if (mem_read32(cpu->mem, addr,
				    &cpu->v[rt].s[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_read32(cpu->mem, addr + 4,
				    &cpu->v[rt2].s[0]) != 0)
					return EMU_SEGFAULT;
				break;
			case 1:	/* 64-bit */
				if (mem_read64(cpu->mem, addr,
				    &cpu->v[rt].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_read64(cpu->mem, addr + 8,
				    &cpu->v[rt2].d[0]) != 0)
					return EMU_SEGFAULT;
				break;
			case 2:	/* 128-bit */
				if (mem_read64(cpu->mem, addr,
				    &cpu->v[rt].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_read64(cpu->mem, addr + 8,
				    &cpu->v[rt].d[1]) != 0)
					return EMU_SEGFAULT;
				if (mem_read64(cpu->mem, addr + 16,
				    &cpu->v[rt2].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_read64(cpu->mem, addr + 24,
				    &cpu->v[rt2].d[1]) != 0)
					return EMU_SEGFAULT;
				break;
			default:
				return EMU_UNIMPL;
			}
		} else {
			/* Store pair */
			switch (opc) {
			case 0:
				if (mem_write32(cpu->mem, addr,
				    cpu->v[rt].s[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_write32(cpu->mem, addr + 4,
				    cpu->v[rt2].s[0]) != 0)
					return EMU_SEGFAULT;
				break;
			case 1:
				if (mem_write64(cpu->mem, addr,
				    cpu->v[rt].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_write64(cpu->mem, addr + 8,
				    cpu->v[rt2].d[0]) != 0)
					return EMU_SEGFAULT;
				break;
			case 2:
				if (mem_write64(cpu->mem, addr,
				    cpu->v[rt].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_write64(cpu->mem, addr + 8,
				    cpu->v[rt].d[1]) != 0)
					return EMU_SEGFAULT;
				if (mem_write64(cpu->mem, addr + 16,
				    cpu->v[rt2].d[0]) != 0)
					return EMU_SEGFAULT;
				if (mem_write64(cpu->mem, addr + 24,
				    cpu->v[rt2].d[1]) != 0)
					return EMU_SEGFAULT;
				break;
			default:
				return EMU_UNIMPL;
			}
		}
	} else {
		/* GPR pair */
		if (L) {
			/* Load pair */
			if (opc == 0) {
				/* 32-bit STP/LDP */
				uint32_t v1, v2;
				if (mem_read32(cpu->mem, addr, &v1) != 0)
					return EMU_SEGFAULT;
				if (mem_read32(cpu->mem, addr + 4, &v2) != 0)
					return EMU_SEGFAULT;
				cpu_set_wreg(cpu, rt, v1);
				cpu_set_wreg(cpu, rt2, v2);
			} else if (opc == 1) {
				/* LDPSW: 32-bit sign-extended to 64 */
				uint32_t v1, v2;
				if (mem_read32(cpu->mem, addr, &v1) != 0)
					return EMU_SEGFAULT;
				if (mem_read32(cpu->mem, addr + 4, &v2) != 0)
					return EMU_SEGFAULT;
				cpu_set_xreg(cpu, rt,
				    (uint64_t)sign_extend(v1, 32));
				cpu_set_xreg(cpu, rt2,
				    (uint64_t)sign_extend(v2, 32));
			} else if (opc == 2) {
				/* 64-bit */
				uint64_t v1, v2;
				if (mem_read64(cpu->mem, addr, &v1) != 0)
					return EMU_SEGFAULT;
				if (mem_read64(cpu->mem, addr + 8, &v2) != 0)
					return EMU_SEGFAULT;
				cpu_set_xreg(cpu, rt, v1);
				cpu_set_xreg(cpu, rt2, v2);
			} else {
				return EMU_UNIMPL;
			}
		} else {
			/* Store pair */
			if (opc == 0) {
				/* 32-bit */
				if (mem_write32(cpu->mem, addr,
				    cpu_wreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				if (mem_write32(cpu->mem, addr + 4,
				    cpu_wreg(cpu, rt2)) != 0)
					return EMU_SEGFAULT;
			} else if (opc == 2) {
				/* 64-bit */
				if (mem_write64(cpu->mem, addr,
				    cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				if (mem_write64(cpu->mem, addr + 8,
				    cpu_xreg(cpu, rt2)) != 0)
					return EMU_SEGFAULT;
			} else {
				return EMU_UNIMPL;
			}
		}
	}

	/* Writeback */
	if (mode == 1) {
		/* Post-index */
		cpu_set_xreg_sp(cpu, rn,
		    (uint64_t)((int64_t)cpu_xreg_sp(cpu, rn) + offset));
	} else if (mode == 3) {
		/* Pre-index */
		cpu_set_xreg_sp(cpu, rn, addr);
	}

	return EMU_OK;
}

static int
exec_ldst_literal(cpu_state_t *cpu, uint32_t insn)
{
	int		opc, V;
	uint32_t	rt;
	int64_t		offset;
	uint64_t	addr;

	opc = bits(insn, 31, 30);
	V = bit(insn, 26);
	rt = bits(insn, 4, 0);

	offset = sign_extend(bits(insn, 23, 5), 19) << 2;
	addr = (uint64_t)((int64_t)cpu->pc + offset);

	if (V) {
		memset(&cpu->v[rt], 0, sizeof(vreg_t));
		switch (opc) {
		case 0:	/* LDR S */
			if (mem_read32(cpu->mem, addr,
			    &cpu->v[rt].s[0]) != 0)
				return EMU_SEGFAULT;
			break;
		case 1:	/* LDR D */
			if (mem_read64(cpu->mem, addr,
			    &cpu->v[rt].d[0]) != 0)
				return EMU_SEGFAULT;
			break;
		case 2:	/* LDR Q */
			if (mem_read64(cpu->mem, addr,
			    &cpu->v[rt].d[0]) != 0)
				return EMU_SEGFAULT;
			if (mem_read64(cpu->mem, addr + 8,
			    &cpu->v[rt].d[1]) != 0)
				return EMU_SEGFAULT;
			break;
		default:
			return EMU_UNIMPL;
		}
	} else {
		switch (opc) {
		case 0: {	/* LDR W */
			uint32_t val;
			if (mem_read32(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_wreg(cpu, rt, val);
			break;
		}
		case 1: {	/* LDR X */
			uint64_t val;
			if (mem_read64(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, val);
			break;
		}
		case 2: {	/* LDRSW */
			uint32_t val;
			if (mem_read32(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt,
			    (uint64_t)sign_extend(val, 32));
			break;
		}
		default:
			return EMU_UNIMPL;
		}
	}

	return EMU_OK;
}

static int
exec_ldst_exclusive(cpu_state_t *cpu, uint32_t insn)
{
	int		size, o2, L, o1, o0;
	uint32_t	rs, rt2, rn, rt;
	uint64_t	addr;

	size = bits(insn, 31, 30);
	o2 = bit(insn, 23);
	L = bit(insn, 22);
	o1 = bit(insn, 21);
	rs = bits(insn, 20, 16);
	o0 = bit(insn, 15);
	rt2 = bits(insn, 14, 10);
	rn = bits(insn, 9, 5);
	rt = bits(insn, 4, 0);

	(void)rt2;	/* pair variants not commonly used */
	(void)o1;

	addr = cpu_xreg_sp(cpu, rn);

	if (o2 == 0 && o0 == 0) {
		/* STXR / STLXR */
		if (L == 0) {
			/* STXR: store exclusive */
			if (!cpu->excl_active ||
			    cpu->excl_addr != addr) {
				/* Exclusive monitor mismatch: fail */
				cpu_set_wreg(cpu, rs, 1);
				cpu->excl_active = 0;
				return EMU_OK;
			}

			switch (size) {
			case 0:
				if (mem_write8(cpu->mem, addr,
				    (uint8_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 1:
				if (mem_write16(cpu->mem, addr,
				    (uint16_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 2:
				if (mem_write32(cpu->mem, addr,
				    cpu_wreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 3:
				if (mem_write64(cpu->mem, addr,
				    cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			}

			cpu_set_wreg(cpu, rs, 0);	/* success */
			cpu->excl_active = 0;
			return EMU_OK;
		}

		/* LDXR: load exclusive */
		cpu->excl_addr = addr;
		cpu->excl_active = 1;

		switch (size) {
		case 0: {
			uint8_t val;
			if (mem_read8(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			cpu->excl_val = val;
			break;
		}
		case 1: {
			uint16_t val;
			if (mem_read16(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			cpu->excl_val = val;
			break;
		}
		case 2: {
			uint32_t val;
			if (mem_read32(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_wreg(cpu, rt, val);
			cpu->excl_val = val;
			break;
		}
		case 3: {
			uint64_t val;
			if (mem_read64(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, val);
			cpu->excl_val = val;
			break;
		}
		}

		return EMU_OK;
	}

	if (o2 == 1 && o0 == 0) {
		/* STLR / LDAR (load-acquire / store-release) */
		if (L == 0) {
			/* STLR */
			switch (size) {
			case 0:
				if (mem_write8(cpu->mem, addr,
				    (uint8_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 1:
				if (mem_write16(cpu->mem, addr,
				    (uint16_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 2:
				if (mem_write32(cpu->mem, addr,
				    cpu_wreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 3:
				if (mem_write64(cpu->mem, addr,
				    cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			}
			return EMU_OK;
		}

		/* LDAR */
		switch (size) {
		case 0: {
			uint8_t val;
			if (mem_read8(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			break;
		}
		case 1: {
			uint16_t val;
			if (mem_read16(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			break;
		}
		case 2: {
			uint32_t val;
			if (mem_read32(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_wreg(cpu, rt, val);
			break;
		}
		case 3: {
			uint64_t val;
			if (mem_read64(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, val);
			break;
		}
		}

		return EMU_OK;
	}

	/* STXR with o0=1: STLXR (store-release exclusive) */
	if (o2 == 0 && o0 == 1) {
		if (L == 0) {
			/* STLXR: same as STXR with release semantics */
			if (!cpu->excl_active ||
			    cpu->excl_addr != addr) {
				cpu_set_wreg(cpu, rs, 1);
				cpu->excl_active = 0;
				return EMU_OK;
			}

			switch (size) {
			case 0:
				if (mem_write8(cpu->mem, addr,
				    (uint8_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 1:
				if (mem_write16(cpu->mem, addr,
				    (uint16_t)cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 2:
				if (mem_write32(cpu->mem, addr,
				    cpu_wreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			case 3:
				if (mem_write64(cpu->mem, addr,
				    cpu_xreg(cpu, rt)) != 0)
					return EMU_SEGFAULT;
				break;
			}

			cpu_set_wreg(cpu, rs, 0);
			cpu->excl_active = 0;
			return EMU_OK;
		}

		/* LDAXR: load-acquire exclusive */
		cpu->excl_addr = addr;
		cpu->excl_active = 1;

		switch (size) {
		case 0: {
			uint8_t val;
			if (mem_read8(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			cpu->excl_val = val;
			break;
		}
		case 1: {
			uint16_t val;
			if (mem_read16(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, (uint64_t)val);
			cpu->excl_val = val;
			break;
		}
		case 2: {
			uint32_t val;
			if (mem_read32(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_wreg(cpu, rt, val);
			cpu->excl_val = val;
			break;
		}
		case 3: {
			uint64_t val;
			if (mem_read64(cpu->mem, addr, &val) != 0)
				return EMU_SEGFAULT;
			cpu_set_xreg(cpu, rt, val);
			cpu->excl_val = val;
			break;
		}
		}

		return EMU_OK;
	}

	LOG_WARN("unimplemented exclusive/ordered at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}

int
exec_ldst(cpu_state_t *cpu, uint32_t insn)
{
	uint32_t	op1;

	op1 = bits(insn, 29, 27);

	/* Load/store pair: bits[29:27] = 101 */
	if (op1 == 5)
		return exec_ldst_pair(cpu, insn);

	/* Load register literal: bits[29:27] = 011, bit[24] = 0 */
	if (op1 == 3 && !bit(insn, 24))
		return exec_ldst_literal(cpu, insn);

	/* Load/store exclusive: bits[29:27] = 010 */
	if (op1 == 2)
		return exec_ldst_exclusive(cpu, insn);

	/* Load/store register variants: bits[29:27] = 111 */
	if (op1 == 7) {
		if (bit(insn, 24)) {
			/* Unsigned offset */
			return exec_ldst_unsigned_offset(cpu, insn);
		}

		/* Check bits[11:10] for sub-type */
		switch (bits(insn, 11, 10)) {
		case 0:	/* Unscaled immediate */
			return exec_ldst_imm9(cpu, insn);
		case 1:	/* Post-index */
			return exec_ldst_imm9(cpu, insn);
		case 2:	/* Register offset (bit[21] must be 1) */
			if (bit(insn, 21))
				return exec_ldst_reg_offset(cpu, insn);
			return exec_ldst_imm9(cpu, insn);
		case 3:	/* Pre-index */
			return exec_ldst_imm9(cpu, insn);
		}
	}

	LOG_WARN("unimplemented ldst at 0x%llx: 0x%08x",
	    (unsigned long long)cpu->pc, insn);
	return EMU_UNIMPL;
}
