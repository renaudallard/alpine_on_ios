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

/*
 * Helper: set up a CPU with a memory space and write an instruction at PC.
 * Uses cpu_step() which reads the instruction from memory.
 * Returns 0 on success.
 */
static int
setup_cpu(cpu_state_t *cpu, uint32_t insn)
{
	cpu_init(cpu);
	cpu->mem = mem_space_create();
	if (!cpu->mem)
		return -1;

	/* Map a page at address 0x10000 for code */
	uint64_t addr = mem_mmap(cpu->mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE | MEM_PROT_EXEC,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);
	if (addr != 0x10000)
		return -1;

	cpu->pc = 0x10000;
	mem_write32(cpu->mem, cpu->pc, insn);

	/* Map a page for stack */
	mem_mmap(cpu->mem, 0x20000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);
	cpu->sp = 0x20000 + 4096;

	return 0;
}

static void
cleanup_cpu(cpu_state_t *cpu)
{
	if (cpu->mem) {
		mem_space_destroy(cpu->mem);
		cpu->mem = NULL;
	}
}

/*
 * Real instruction encodings from aarch64-linux-gnu-as.
 *
 * movz x0, #0x1234           -> 0xd2824680
 * movz x1, #0xABCD, lsl #16 -> 0xd2b579a1
 * movk x2, #0x5678           -> 0xf28acf02
 * movn x3, #0                -> 0x92800003
 * add x4, x5, #100           -> 0x910190a4
 * sub x6, x7, #10            -> 0xd10028e6
 * b . + 0x100                -> 0x14000040 (at offset 0x18 in .text)
 * bl . + 0x200               -> 0x94000080 (at offset 0x1c)
 * b.eq . + 0x40              -> 0x54000200 (at offset 0x20)
 * cbz x5, . + 0x20           -> 0xb4000105 (at offset 0x24)
 * cbnz x7, . + 0x30          -> 0xb5000187 (at offset 0x28)
 */

static void
test_movz(void)
{
	cpu_state_t cpu;

	test_begin("MOVZ x0, #0x1234");
	ASSERT_EQ(setup_cpu(&cpu, 0xd2824680), 0);

	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[0], 0x1234ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_movz_shifted(void)
{
	cpu_state_t cpu;

	test_begin("MOVZ x1, #0xABCD, LSL #16");
	ASSERT_EQ(setup_cpu(&cpu, 0xd2b579a1), 0);

	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[1], 0xABCD0000ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_movk(void)
{
	cpu_state_t cpu;

	test_begin("MOVK x2, #0x5678 keeps other bits");
	ASSERT_EQ(setup_cpu(&cpu, 0xf28acf02), 0);

	cpu.x[2] = 0xFFFF0000ULL;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[2], 0xFFFF5678ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_movn(void)
{
	cpu_state_t cpu;

	test_begin("MOVN x3, #0 -> all ones");
	ASSERT_EQ(setup_cpu(&cpu, 0x92800003), 0);

	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[3], 0xFFFFFFFFFFFFFFFFULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_add_imm(void)
{
	cpu_state_t cpu;

	test_begin("ADD x4, x5, #100");
	ASSERT_EQ(setup_cpu(&cpu, 0x910190a4), 0);

	cpu.x[5] = 42;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[4], 142ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_sub_imm(void)
{
	cpu_state_t cpu;

	test_begin("SUB x6, x7, #10");
	ASSERT_EQ(setup_cpu(&cpu, 0xd10028e6), 0);

	cpu.x[7] = 100;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.x[6], 90ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_b_forward(void)
{
	cpu_state_t cpu;

	/* b . + 0x100 -> 0x14000040
	 * imm26 = 0x40 -> offset = 0x40 * 4 = 0x100 */
	test_begin("B forward +0x100");
	ASSERT_EQ(setup_cpu(&cpu, 0x14000040), 0);

	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 0x100ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_bl(void)
{
	cpu_state_t cpu;

	/* bl . + 0x200 -> 0x94000080
	 * imm26 = 0x80 -> offset = 0x80 * 4 = 0x200 */
	test_begin("BL sets X30 to return address");
	ASSERT_EQ(setup_cpu(&cpu, 0x94000080), 0);

	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 0x200ULL);
	ASSERT_EQ(cpu.x[30], 0x10000ULL + 4ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_bcond_taken(void)
{
	cpu_state_t cpu;

	/* b.eq . + 0x40 -> 0x54000200
	 * imm19 = 0x10 -> offset = 0x10 * 4 = 0x40 */
	test_begin("B.EQ taken when Z set");
	ASSERT_EQ(setup_cpu(&cpu, 0x54000200), 0);

	cpu.nzcv = PSTATE_Z;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 0x40ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_bcond_not_taken(void)
{
	cpu_state_t cpu;

	test_begin("B.EQ not taken when Z clear");
	ASSERT_EQ(setup_cpu(&cpu, 0x54000200), 0);

	cpu.nzcv = 0;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 4ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_cbz_taken(void)
{
	cpu_state_t cpu;

	/* cbz x5, . + 0x20 -> 0xb4000105
	 * imm19 = 0x8 -> offset = 0x8 * 4 = 0x20 */
	test_begin("CBZ taken when x5 == 0");
	ASSERT_EQ(setup_cpu(&cpu, 0xb4000105), 0);

	cpu.x[5] = 0;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 0x20ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_cbz_not_taken(void)
{
	cpu_state_t cpu;

	test_begin("CBZ not taken when x5 != 0");
	ASSERT_EQ(setup_cpu(&cpu, 0xb4000105), 0);

	cpu.x[5] = 1;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 4ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

static void
test_cbnz(void)
{
	cpu_state_t cpu;

	/* cbnz x7, . + 0x30 -> 0xb5000187
	 * imm19 = 0xc -> offset = 0xc * 4 = 0x30 */
	test_begin("CBNZ taken when x7 != 0");
	ASSERT_EQ(setup_cpu(&cpu, 0xb5000187), 0);

	cpu.x[7] = 42;
	int rc = cpu_step(&cpu);
	ASSERT_EQ(rc, EMU_OK);
	ASSERT_EQ(cpu.pc, 0x10000ULL + 0x30ULL);

	cleanup_cpu(&cpu);
	test_pass();
}

void
test_suite_decoder(void)
{
	test_movz();
	test_movz_shifted();
	test_movk();
	test_movn();
	test_add_imm();
	test_sub_imm();
	test_b_forward();
	test_bl();
	test_bcond_taken();
	test_bcond_not_taken();
	test_cbz_taken();
	test_cbz_not_taken();
	test_cbnz();
}
