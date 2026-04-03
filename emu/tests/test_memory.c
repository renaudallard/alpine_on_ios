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
test_mem_create_destroy(void)
{
	mem_space_t *mem;

	test_begin("mem_space_create/destroy");
	mem = mem_space_create();
	ASSERT(mem != NULL);
	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_mmap_basic(void)
{
	mem_space_t *mem;
	uint64_t addr;

	test_begin("mem_mmap basic allocation");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	addr = mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);
	ASSERT_EQ(addr, 0x10000ULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_translate(void)
{
	mem_space_t *mem;
	uint64_t addr;
	void *host;

	test_begin("mem_translate returns valid host pointer");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	addr = mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);
	ASSERT_EQ(addr, 0x10000ULL);

	host = mem_translate(mem, 0x10000, 4, MEM_PROT_READ);
	ASSERT(host != NULL);

	/* Unmapped address should return NULL */
	host = mem_translate(mem, 0x99999, 4, MEM_PROT_READ);
	ASSERT(host == NULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_read_write_8(void)
{
	mem_space_t *mem;
	uint8_t val;

	test_begin("mem_read8/write8");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	ASSERT_EQ(mem_write8(mem, 0x10000, 0xAB), 0);
	ASSERT_EQ(mem_read8(mem, 0x10000, &val), 0);
	ASSERT_EQ(val, 0xAB);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_read_write_16(void)
{
	mem_space_t *mem;
	uint16_t val;

	test_begin("mem_read16/write16");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	ASSERT_EQ(mem_write16(mem, 0x10000, 0xBEEF), 0);
	ASSERT_EQ(mem_read16(mem, 0x10000, &val), 0);
	ASSERT_EQ(val, 0xBEEF);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_read_write_32(void)
{
	mem_space_t *mem;
	uint32_t val;

	test_begin("mem_read32/write32");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	ASSERT_EQ(mem_write32(mem, 0x10000, 0xDEADBEEF), 0);
	ASSERT_EQ(mem_read32(mem, 0x10000, &val), 0);
	ASSERT_EQ(val, 0xDEADBEEFU);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_read_write_64(void)
{
	mem_space_t *mem;
	uint64_t val;

	test_begin("mem_read64/write64");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	ASSERT_EQ(mem_write64(mem, 0x10000, 0xCAFEBABEDEADC0DEULL), 0);
	ASSERT_EQ(mem_read64(mem, 0x10000, &val), 0);
	ASSERT_EQ(val, 0xCAFEBABEDEADC0DEULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_brk(void)
{
	mem_space_t *mem;
	uint64_t brk;

	test_begin("mem_brk");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	/* Set initial brk */
	mem->brk_base = 0x100000;
	mem->brk_current = 0x100000;

	/* Query current brk */
	brk = mem_brk(mem, 0);
	ASSERT_EQ(brk, 0x100000ULL);

	/* Extend brk */
	brk = mem_brk(mem, 0x101000);
	ASSERT_EQ(brk, 0x101000ULL);
	ASSERT_EQ(mem->brk_current, 0x101000ULL);

	/* Should not go below base */
	brk = mem_brk(mem, 0x50000);
	ASSERT_EQ(brk, 0x101000ULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_munmap(void)
{
	mem_space_t *mem;

	test_begin("mem_munmap");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	uint64_t addr = mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);
	ASSERT_EQ(addr, 0x10000ULL);

	/* Should be accessible */
	ASSERT(mem_translate(mem, 0x10000, 1, MEM_PROT_READ) != NULL);

	/* Unmap */
	ASSERT_EQ(mem_munmap(mem, 0x10000, 4096), 0);

	/* Should no longer be accessible */
	ASSERT(mem_translate(mem, 0x10000, 1, MEM_PROT_READ) == NULL);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_copy_to_from(void)
{
	mem_space_t *mem;
	const char *src = "Hello, Alpine!";
	char dst[32];

	test_begin("mem_copy_to/from");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	ASSERT_EQ(mem_copy_to(mem, 0x10000, src, strlen(src) + 1), 0);

	memset(dst, 0, sizeof(dst));
	ASSERT_EQ(mem_copy_from(mem, dst, 0x10000, strlen(src) + 1), 0);
	ASSERT_EQ(strcmp(dst, "Hello, Alpine!"), 0);

	mem_space_destroy(mem);
	test_pass();
}

static void
test_mem_read_str(void)
{
	mem_space_t *mem;
	char buf[64];

	test_begin("mem_read_str");
	mem = mem_space_create();
	ASSERT(mem != NULL);

	mem_mmap(mem, 0x10000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	const char *test = "/bin/sh";
	mem_copy_to(mem, 0x10000, test, strlen(test) + 1);

	memset(buf, 0, sizeof(buf));
	ASSERT_EQ(mem_read_str(mem, 0x10000, buf, sizeof(buf)), 0);
	ASSERT_EQ(strcmp(buf, "/bin/sh"), 0);

	mem_space_destroy(mem);
	test_pass();
}

void
test_suite_memory(void)
{
	test_mem_create_destroy();
	test_mem_mmap_basic();
	test_mem_translate();
	test_mem_read_write_8();
	test_mem_read_write_16();
	test_mem_read_write_32();
	test_mem_read_write_64();
	test_mem_brk();
	test_mem_munmap();
	test_mem_copy_to_from();
	test_mem_read_str();
}
