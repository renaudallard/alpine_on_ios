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
#include <unistd.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include "cpu.h"
#include "memory.h"
#include "process.h"
#include "syscall.h"
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
 * Helper: create a minimal process for syscall testing.
 * Returns NULL on failure.
 */
static emu_process_t *
create_test_process(void)
{
	static int initialized;
	emu_process_t *proc;

	if (!initialized) {
		proc_table_init();
		initialized = 1;
	}

	proc = proc_create(NULL);
	if (!proc)
		return NULL;

	/* Set up memory space */
	proc->mem = mem_space_create();
	if (!proc->mem) {
		proc_destroy(proc);
		return NULL;
	}

	/* Map a page for syscall data buffers */
	mem_mmap(proc->mem, 0x100000, 4096,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS | MEM_MAP_FIXED,
	    -1, 0);

	cpu_init(&proc->cpu);
	proc->cpu.mem = proc->mem;

	return proc;
}

static void
destroy_test_process(emu_process_t *proc)
{
	/* proc_destroy handles freeing proc->mem and proc->fds */
	proc_destroy(proc);
}

static void
test_sys_uname(void)
{
	emu_process_t *proc;
	int64_t ret;

	test_begin("sys_misc uname");
	proc = create_test_process();
	ASSERT(proc != NULL);

	/* uname writes to a buffer in guest memory.
	 * struct utsname is 6 * 65 bytes on aarch64 linux. */
	uint64_t buf_addr = 0x100000;

	ret = sys_misc(proc, SYS_UNAME,
	    buf_addr, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, 0LL);

	/* Read sysname from the buffer (first 65 bytes) */
	char sysname[65];
	memset(sysname, 0, sizeof(sysname));
	mem_copy_from(proc->mem, sysname, buf_addr, 64);
	sysname[64] = '\0';

	/* Should report "Linux" */
	ASSERT_EQ(strcmp(sysname, "Linux"), 0);

	destroy_test_process(proc);
	test_pass();
}

static void
test_sys_brk(void)
{
	emu_process_t *proc;
	int64_t ret;

	test_begin("sys_memory brk");
	proc = create_test_process();
	ASSERT(proc != NULL);

	/* Set up a brk region */
	proc->mem->brk_base = 0x200000;
	proc->mem->brk_current = 0x200000;

	/* Query current brk (arg = 0) */
	ret = sys_memory(proc, SYS_BRK, 0, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, 0x200000LL);

	/* Extend brk */
	ret = sys_memory(proc, SYS_BRK, 0x201000, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, 0x201000LL);

	destroy_test_process(proc);
	test_pass();
}

static void
test_sys_getpid(void)
{
	emu_process_t *proc;
	int64_t ret;

	test_begin("sys_process getpid");
	proc = create_test_process();
	ASSERT(proc != NULL);

	ret = sys_process(proc, SYS_GETPID, 0, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, (int64_t)proc->pid);

	destroy_test_process(proc);
	test_pass();
}

static void
test_sys_getuid(void)
{
	emu_process_t *proc;
	int64_t ret;

	test_begin("sys_process getuid/geteuid");
	proc = create_test_process();
	ASSERT(proc != NULL);

	proc->uid = 0;
	proc->euid = 0;
	ret = sys_process(proc, SYS_GETUID, 0, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, 0LL);

	ret = sys_process(proc, SYS_GETEUID, 0, 0, 0, 0, 0, 0);
	ASSERT_EQ(ret, 0LL);

	destroy_test_process(proc);
	test_pass();
}

static void
test_sys_file_basic(void)
{
	emu_process_t *proc;

	test_begin("sys_file open/write/read/close");
	proc = create_test_process();
	ASSERT(proc != NULL);

	/* Create a VFS for the process to use with file syscalls */
	proc->vfs = vfs_create(".");
	ASSERT(proc->vfs != NULL);

	/* Set up a fd table */
	proc->fds = fd_table_create();
	ASSERT(proc->fds != NULL);

	/* We test the fd_table layer directly since sys_file needs
	 * a full VFS setup which depends on rootfs. */
	int fd = fd_alloc(proc->fds, 0);
	ASSERT(fd >= 0);

	/* fd_alloc reserves the slot but leaves type as FD_NONE.
	 * Set it up directly, then verify fd_get finds it. */
	proc->fds->fds[fd].type = FD_FILE;
	proc->fds->fds[fd].real_fd = -1;

	fd_entry_t *entry = fd_get(proc->fds, fd);
	ASSERT(entry != NULL);
	ASSERT_EQ(entry->type, FD_FILE);

	/* Close it */
	fd_close(proc->fds, fd);
	entry = fd_get(proc->fds, fd);
	ASSERT(entry == NULL);

	vfs_destroy(proc->vfs);
	proc->vfs = NULL;
	fd_table_release(proc->fds);
	proc->fds = NULL;

	destroy_test_process(proc);
	test_pass();
}

void
test_suite_syscall(void)
{
	test_sys_uname();
	test_sys_brk();
	test_sys_getpid();
	test_sys_getuid();
	test_sys_file_basic();
}
