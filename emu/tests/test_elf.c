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

#include <stdio.h>
#include <string.h>
#include "elf_loader.h"
#include "memory.h"

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
test_elf_load_nonexistent(void)
{
	mem_space_t *mem;
	elf_info_t info;

	test_begin("elf_load rejects nonexistent file");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	memset(&info, 0, sizeof(info));
	ASSERT_EQ(elf_load("/nonexistent/path", mem, 0, &info), -1);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_elf_load_invalid(void)
{
	mem_space_t *mem;
	elf_info_t info;
	FILE *f;
	const char *path = "build/tests/invalid.elf";

	test_begin("elf_load rejects invalid ELF");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	/* Create a file with invalid ELF magic */
	f = fopen(path, "wb");
	ASSERT(f != NULL);
	const char garbage[] = "NOT_AN_ELF_FILE";
	fwrite(garbage, 1, sizeof(garbage), f);
	fclose(f);

	memset(&info, 0, sizeof(info));
	ASSERT_EQ(elf_load(path, mem, 0, &info), -1);

	remove(path);
	mem_space_destroy(mem);
	test_pass();
}

static void
test_elf_setup_stack(void)
{
	mem_space_t *mem;
	elf_info_t info;
	uint64_t sp;

	test_begin("elf_setup_stack lays out argc/argv");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	/* Map a stack region */
	uint64_t stack_top = 0x800000;
	uint64_t stack_base = stack_top - 0x10000;
	mem_mmap(mem, stack_base, 0x10000,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	memset(&info, 0, sizeof(info));
	info.entry = 0x400000;

	const char *argv[] = { "/bin/sh", "-l", NULL };
	const char *envp[] = { "HOME=/root", NULL };

	sp = elf_setup_stack(mem, &info, argv, envp, stack_top);

	/* Stack pointer should be below stack_top and aligned to 16 bytes */
	ASSERT(sp < stack_top);
	ASSERT(sp > stack_base);
	ASSERT_EQ(sp & 0xF, 0ULL);

	/* First value on stack should be argc = 2 */
	uint64_t argc;
	ASSERT_EQ(mem_read64(mem, sp, &argc), 0);
	ASSERT_EQ(argc, 2ULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_elf_info_struct(void)
{
	elf_info_t info;

	test_begin("elf_info_t zeroes correctly");
	memset(&info, 0xFF, sizeof(info));
	memset(&info, 0, sizeof(info));

	ASSERT_EQ(info.entry, 0ULL);
	ASSERT_EQ(info.phdr, 0ULL);
	ASSERT_EQ(info.phnum, 0ULL);
	ASSERT_EQ(info.base, 0ULL);
	ASSERT_EQ(info.interp_base, 0ULL);
	ASSERT_EQ(info.interp_entry, 0ULL);
	ASSERT_EQ(info.interp[0], '\0');

	test_pass();
}

void
test_suite_elf(void)
{
	test_elf_load_nonexistent();
	test_elf_load_invalid();
	test_elf_setup_stack();
	test_elf_info_struct();
}
