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

/*
 * Apply shift operation to a 64-bit value.
 * shift_type: 0=LSL, 1=LSR, 2=ASR, 3=ROR
 */
uint64_t
apply_shift(uint64_t val, int shift_type, int amount)
{
	if (amount == 0)
		return val;

	switch (shift_type) {
	case 0:	/* LSL */
		return val << amount;
	case 1:	/* LSR */
		return val >> amount;
	case 2:	/* ASR */
		return (uint64_t)((int64_t)val >> amount);
	case 3:	/* ROR */
		return (val >> amount) | (val << (64 - amount));
	default:
		return val;
	}
}

/*
 * Apply shift operation to a 32-bit value.
 */
uint32_t
apply_shift32(uint32_t val, int shift_type, int amount)
{
	if (amount == 0)
		return val;

	switch (shift_type) {
	case 0:	/* LSL */
		return val << amount;
	case 1:	/* LSR */
		return val >> amount;
	case 2:	/* ASR */
		return (uint32_t)((int32_t)val >> amount);
	case 3:	/* ROR */
		return (val >> amount) | (val << (32 - amount));
	default:
		return val;
	}
}

/*
 * Extended register value for add/sub extended register instructions.
 * ext_type: 000=UXTB, 001=UXTH, 010=UXTW, 011=UXTX,
 *           100=SXTB, 101=SXTH, 110=SXTW, 111=SXTX
 */
uint64_t
extend_reg(cpu_state_t *cpu, int reg, int ext_type, int shift)
{
	uint64_t	val;

	val = cpu_xreg(cpu, reg);

	switch (ext_type) {
	case 0:	/* UXTB */
		val = val & 0xFF;
		break;
	case 1:	/* UXTH */
		val = val & 0xFFFF;
		break;
	case 2:	/* UXTW */
		val = val & 0xFFFFFFFF;
		break;
	case 3:	/* UXTX */
		break;
	case 4:	/* SXTB */
		val = (uint64_t)sign_extend(val, 8);
		break;
	case 5:	/* SXTH */
		val = (uint64_t)sign_extend(val, 16);
		break;
	case 6:	/* SXTW */
		val = (uint64_t)sign_extend(val, 32);
		break;
	case 7:	/* SXTX */
		break;
	default:
		break;
	}

	return val << shift;
}
