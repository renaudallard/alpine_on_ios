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
#include <stdlib.h>
#include <string.h>

/* Simple test framework */
int test_passed;
int test_failed;
int test_total;
const char *current_test;

void
test_begin(const char *name)
{
	current_test = name;
	test_total++;
}

void
test_pass(void)
{
	printf("  PASS: %s\n", current_test);
	test_passed++;
}

void
test_fail(const char *file, int line, const char *expr)
{
	printf("  FAIL: %s (%s:%d: %s)\n", current_test, file, line, expr);
	test_failed++;
}

/* Test suite declarations */
void test_suite_cpu(void);
void test_suite_decoder(void);
void test_suite_memory(void);
void test_suite_elf(void);
void test_suite_syscall(void);

int
main(void)
{
	test_passed = 0;
	test_failed = 0;
	test_total = 0;

	printf("Running emulator tests...\n\n");

	printf("[CPU]\n");
	test_suite_cpu();
	printf("\n");

	printf("[Decoder]\n");
	test_suite_decoder();
	printf("\n");

	printf("[Memory]\n");
	test_suite_memory();
	printf("\n");

	printf("[ELF]\n");
	test_suite_elf();
	printf("\n");

	printf("[Syscall]\n");
	test_suite_syscall();
	printf("\n");

	printf("Results: %d passed, %d failed, %d total\n",
	    test_passed, test_failed, test_total);

	return test_failed > 0 ? 1 : 0;
}
