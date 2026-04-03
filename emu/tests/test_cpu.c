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

/* Test framework from test_main.c */
extern void test_begin(const char *name);
extern void test_pass(void);
extern void test_fail(const char *file, int line, const char *expr);

#define ASSERT(cond) do { \
	if (!(cond)) { test_fail(__FILE__, __LINE__, #cond); return; } \
} while (0)

#define ASSERT_EQ(a, b) do { \
	if ((a) != (b)) { test_fail(__FILE__, __LINE__, #a " == " #b); return; } \
} while (0)

static void
test_cpu_init_zeroes(void)
{
	cpu_state_t cpu;

	test_begin("cpu_init zeroes registers");
	cpu_init(&cpu);

	for (int i = 0; i < 31; i++)
		ASSERT_EQ(cpu.x[i], 0ULL);
	ASSERT_EQ(cpu.sp, 0ULL);
	ASSERT_EQ(cpu.pc, 0ULL);
	ASSERT_EQ(cpu.nzcv, 0U);
	ASSERT_EQ(cpu.fpcr, 0U);
	ASSERT_EQ(cpu.fpsr, 0U);
	ASSERT_EQ(cpu.tpidr_el0, 0ULL);
	ASSERT_EQ(cpu.excl_active, 0);

	test_pass();
}

static void
test_cpu_xreg_zero(void)
{
	cpu_state_t cpu;

	test_begin("cpu_xreg r31 returns zero");
	cpu_init(&cpu);

	/* R31 as zero register should always return 0 */
	cpu.x[0] = 42;
	ASSERT_EQ(cpu_xreg(&cpu, 0), 42ULL);
	ASSERT_EQ(cpu_xreg(&cpu, 31), 0ULL);
	ASSERT_EQ(cpu_wreg(&cpu, 31), 0U);

	/* R31 as SP register should return sp */
	cpu.sp = 0x1000;
	ASSERT_EQ(cpu_xreg_sp(&cpu, 31), 0x1000ULL);
	ASSERT_EQ(cpu_xreg_sp(&cpu, 0), 42ULL);

	test_pass();
}

static void
test_cpu_set_xreg(void)
{
	cpu_state_t cpu;

	test_begin("cpu_set_xreg ignores r31");
	cpu_init(&cpu);

	cpu_set_xreg(&cpu, 5, 0xDEAD);
	ASSERT_EQ(cpu.x[5], 0xDEADULL);

	/* Writing to r31 should be a no-op */
	cpu_set_xreg(&cpu, 31, 0xBEEF);
	/* No crash, no effect on sp */
	ASSERT_EQ(cpu.sp, 0ULL);

	/* Writing to r31 as SP should set sp */
	cpu_set_xreg_sp(&cpu, 31, 0xCAFE);
	ASSERT_EQ(cpu.sp, 0xCAFEULL);

	test_pass();
}

static void
test_cpu_wreg_zero_extend(void)
{
	cpu_state_t cpu;

	test_begin("cpu_set_wreg zero extends");
	cpu_init(&cpu);

	cpu.x[3] = 0xFFFFFFFFFFFFFFFFULL;
	cpu_set_wreg(&cpu, 3, 0x12345678);
	ASSERT_EQ(cpu.x[3], 0x12345678ULL);

	test_pass();
}

static void
test_cpu_flags_add32(void)
{
	cpu_state_t cpu;

	test_begin("cpu_update_flags_add32");
	cpu_init(&cpu);

	/* 0 + 0 = 0 -> Z set */
	cpu_update_flags_add32(&cpu, 0, 0, 0);
	ASSERT(cpu.nzcv & PSTATE_Z);
	ASSERT(!(cpu.nzcv & PSTATE_N));
	ASSERT(!(cpu.nzcv & PSTATE_C));
	ASSERT(!(cpu.nzcv & PSTATE_V));

	/* 0x7FFFFFFF + 1 = 0x80000000 -> N set, V set */
	cpu_update_flags_add32(&cpu, 0x7FFFFFFF, 1, 0x80000000U);
	ASSERT(cpu.nzcv & PSTATE_N);
	ASSERT(!(cpu.nzcv & PSTATE_Z));
	ASSERT(!(cpu.nzcv & PSTATE_C));
	ASSERT(cpu.nzcv & PSTATE_V);

	/* 0xFFFFFFFF + 1 = 0 -> Z set, C set */
	cpu_update_flags_add32(&cpu, 0xFFFFFFFF, 1, 0);
	ASSERT(cpu.nzcv & PSTATE_Z);
	ASSERT(cpu.nzcv & PSTATE_C);

	test_pass();
}

static void
test_cpu_flags_sub64(void)
{
	cpu_state_t cpu;

	test_begin("cpu_update_flags_sub64");
	cpu_init(&cpu);

	/* 5 - 5 = 0 -> Z set, C set (no borrow) */
	cpu_update_flags_sub64(&cpu, 5, 5, 0);
	ASSERT(cpu.nzcv & PSTATE_Z);
	ASSERT(cpu.nzcv & PSTATE_C);
	ASSERT(!(cpu.nzcv & PSTATE_V));

	/* 0 - 1 = -1 (0xFFFFFFFFFFFFFFFF) -> N set, no C (borrow) */
	cpu_update_flags_sub64(&cpu, 0, 1, 0xFFFFFFFFFFFFFFFFULL);
	ASSERT(cpu.nzcv & PSTATE_N);
	ASSERT(!(cpu.nzcv & PSTATE_C));

	test_pass();
}

static void
test_cpu_cond_check(void)
{
	cpu_state_t cpu;

	test_begin("cpu_check_cond");
	cpu_init(&cpu);

	/* Set Z flag -> EQ should pass, NE should fail */
	cpu.nzcv = PSTATE_Z;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_EQ), 1);
	ASSERT_EQ(cpu_check_cond(&cpu, COND_NE), 0);

	/* Set N flag -> MI should pass, PL should fail */
	cpu.nzcv = PSTATE_N;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_MI), 1);
	ASSERT_EQ(cpu_check_cond(&cpu, COND_PL), 0);

	/* Set C flag -> CS should pass, CC should fail */
	cpu.nzcv = PSTATE_C;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_CS), 1);
	ASSERT_EQ(cpu_check_cond(&cpu, COND_CC), 0);

	/* Set V flag -> VS should pass, VC should fail */
	cpu.nzcv = PSTATE_V;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_VS), 1);
	ASSERT_EQ(cpu_check_cond(&cpu, COND_VC), 0);

	/* AL always passes */
	cpu.nzcv = 0;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_AL), 1);

	/* GE: N == V */
	cpu.nzcv = PSTATE_N | PSTATE_V;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_GE), 1);
	cpu.nzcv = PSTATE_N;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_GE), 0);
	cpu.nzcv = 0;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_GE), 1);

	/* GT: !Z && N == V */
	cpu.nzcv = 0;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_GT), 1);
	cpu.nzcv = PSTATE_Z;
	ASSERT_EQ(cpu_check_cond(&cpu, COND_GT), 0);

	test_pass();
}

static void
test_cpu_bitmask_imm(void)
{
	uint64_t val;

	test_begin("decode_bitmask_imm known values");

	/* 32-bit, N=0, immr=0, imms=0 -> single low bit = 0x1 */
	ASSERT_EQ(decode_bitmask_imm(0, 0, 0, 0, &val), 0);
	ASSERT_EQ(val, 0x00000001ULL);

	/* 64-bit, N=1, immr=0, imms=0 -> single low bit = 0x1 */
	ASSERT_EQ(decode_bitmask_imm(1, 1, 0, 0, &val), 0);
	ASSERT_EQ(val, 0x0000000000000001ULL);

	/* 64-bit, N=1, immr=0, imms=62 -> 63 low bits set */
	ASSERT_EQ(decode_bitmask_imm(1, 1, 0, 62, &val), 0);
	ASSERT_EQ(val, 0x7FFFFFFFFFFFFFFFULL);

	/* imms=63 with N=1 is reserved (all ones) -> must return -1 */
	ASSERT_EQ(decode_bitmask_imm(1, 1, 0, 63, &val), -1);

	/* 32-bit with imms=31 (all 32-bit ones) is reserved */
	ASSERT_EQ(decode_bitmask_imm(0, 0, 0, 31, &val), -1);

	test_pass();
}

static void
test_cpu_bit_helpers(void)
{
	test_begin("bit extraction helpers");

	ASSERT_EQ(bits(0xABCDEF01, 31, 28), 0xAU);
	ASSERT_EQ(bits(0xABCDEF01, 7, 0), 0x01U);
	ASSERT_EQ(bit(0x80000000, 31), 1U);
	ASSERT_EQ(bit(0x80000000, 0), 0U);

	ASSERT_EQ(sign_extend(0xFF, 8), -1LL);
	ASSERT_EQ(sign_extend(0x7F, 8), 127LL);
	ASSERT_EQ(sign_extend(0x80, 8), -128LL);

	test_pass();
}

void
test_suite_cpu(void)
{
	test_cpu_init_zeroes();
	test_cpu_xreg_zero();
	test_cpu_set_xreg();
	test_cpu_wreg_zero_extend();
	test_cpu_flags_add32();
	test_cpu_flags_sub64();
	test_cpu_cond_check();
	test_cpu_bitmask_imm();
	test_cpu_bit_helpers();
}
