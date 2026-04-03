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

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "framebuffer.h"
#include "log.h"

/* Linux errno */
#define LINUX_ENOMEM	12
#define LINUX_EFAULT	14
#define LINUX_EINVAL	22
#define LINUX_ENOSYS	38

/* Linux MREMAP flags */
#define LINUX_MREMAP_MAYMOVE	1

/* Linux MAP flags (to detect ANONYMOUS) */
#define LINUX_MAP_ANONYMOUS	0x20

static int64_t
do_brk(emu_process_t *proc, uint64_t a0)
{
	uint64_t	ret;

	ret = mem_brk(proc->mem, a0);

	LOG_TRACE("brk: req=0x%llx ret=0x%llx",
	    (unsigned long long)a0, (unsigned long long)ret);

	return (int64_t)ret;
}

static int64_t
do_mmap(emu_process_t *proc, uint64_t addr, uint64_t length, uint64_t prot,
    uint64_t flags, uint64_t fd_arg, uint64_t offset)
{
	int		real_fd;
	uint64_t	ret;

	real_fd = -1;
	if (!(flags & LINUX_MAP_ANONYMOUS) && (int)(int32_t)fd_arg >= 0) {
		fd_entry_t	*fde;
		int		 efd;

		efd = (int)(int32_t)fd_arg;
		fde = fd_get(proc->fds, efd);
		if (fde == NULL || fde->type == FD_NONE) {
			/* bad fd */
		} else if (fde->type == FD_FB) {
			/*
			 * Framebuffer mmap: allocate a guest region
			 * backed by the shared fb pixel buffer.
			 */
			framebuffer_t	*fb;
			uint64_t	fb_len;

			fb = fb_get();
			if (!fb->active)
				return -LINUX_ENOMEM;
			fb_len = fb->size;
			if (length > fb_len)
				length = fb_len;
			ret = mem_mmap(proc->mem, addr, length,
			    (int)prot,
			    (int)(flags | LINUX_MAP_ANONYMOUS),
			    -1, 0);
			if (ret != (uint64_t)-1) {
				void	*dst;

				dst = mem_translate(proc->mem, ret,
				    length, MEM_PROT_WRITE);
				if (dst != NULL)
					memcpy(dst, fb->pixels, length);
				/*
				 * Store the guest address so the fb
				 * pixel data can be synced. For now
				 * the guest writes directly.
				 */
			}
			LOG_TRACE("mmap(fb): ret=0x%llx len=0x%llx",
			    (unsigned long long)ret,
			    (unsigned long long)length);
			return (int64_t)ret;
		} else {
			real_fd = fde->real_fd;
		}
	}

	ret = mem_mmap(proc->mem, addr, length, (int)prot, (int)flags,
	    real_fd, offset);

	LOG_TRACE("mmap: addr=0x%llx len=0x%llx prot=%llu flags=0x%llx "
	    "fd=%lld off=0x%llx ret=0x%llx",
	    (unsigned long long)addr, (unsigned long long)length,
	    (unsigned long long)prot, (unsigned long long)flags,
	    (long long)(int32_t)fd_arg, (unsigned long long)offset,
	    (unsigned long long)ret);

	return (int64_t)ret;
}

static int64_t
do_munmap(emu_process_t *proc, uint64_t addr, uint64_t length)
{
	return mem_munmap(proc->mem, addr, length);
}

static int64_t
do_mprotect(emu_process_t *proc, uint64_t addr, uint64_t length,
    uint64_t prot)
{
	return mem_mprotect(proc->mem, addr, length, (int)prot);
}

static int64_t
do_mremap(emu_process_t *proc, uint64_t old_addr, uint64_t old_size,
    uint64_t new_size, uint64_t flags)
{
	uint64_t	new_addr;
	void		*old_data;

	if (new_size == 0)
		return -LINUX_EINVAL;

	if (new_size <= old_size) {
		/* Shrink: unmap the tail. */
		if (new_size < old_size)
			mem_munmap(proc->mem, old_addr + new_size,
			    old_size - new_size);
		return (int64_t)old_addr;
	}

	if (!(flags & LINUX_MREMAP_MAYMOVE)) {
		/*
		 * Cannot move. Try to extend in-place by mapping
		 * the extra region right after the old one.
		 */
		uint64_t	extra;

		extra = new_size - old_size;
		if (mem_mmap(proc->mem, old_addr + old_size, extra,
		    MEM_PROT_READ | MEM_PROT_WRITE,
		    MEM_MAP_PRIVATE | MEM_MAP_FIXED | MEM_MAP_ANONYMOUS,
		    -1, 0) == (uint64_t)-1)
			return -LINUX_ENOMEM;
		return (int64_t)old_addr;
	}

	/* Allocate new region. */
	new_addr = mem_mmap(proc->mem, 0, new_size,
	    MEM_PROT_READ | MEM_PROT_WRITE,
	    MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS, -1, 0);
	if (new_addr == (uint64_t)-1)
		return -LINUX_ENOMEM;

	/* Copy old data. */
	old_data = mem_translate(proc->mem, old_addr, old_size,
	    MEM_PROT_READ);
	if (old_data != NULL) {
		void	*new_data;

		new_data = mem_translate(proc->mem, new_addr, old_size,
		    MEM_PROT_WRITE);
		if (new_data != NULL)
			memcpy(new_data, old_data, old_size);
	}

	/* Unmap old region. */
	mem_munmap(proc->mem, old_addr, old_size);

	return (int64_t)new_addr;
}

static int64_t
do_memfd_create(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		hfd, efd;
	fd_entry_t	*fde;
	char		name[256];

	(void)a1;	/* flags */

	if (mem_read_str(proc->mem, a0, name, sizeof(name)) != 0)
		return -LINUX_EFAULT;

#ifdef __linux__
	extern int memfd_create(const char *, unsigned int);
	hfd = memfd_create(name, 0);
#else
	/* Fallback: use a temp file in the sandbox temp directory. */
	const char	*tmpdir;
	char		 tmpl[PATH_MAX];

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	snprintf(tmpl, sizeof(tmpl), "%s/emu_memfd_XXXXXX", tmpdir);
	hfd = mkstemp(tmpl);
	if (hfd >= 0)
		unlink(tmpl);
#endif
	if (hfd < 0)
		return -LINUX_ENOMEM;

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(hfd);
		return -LINUX_ENOMEM;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_FILE;
	fde->real_fd = hfd;
	fde->flags = 0;
	fde->cloexec = 0;

	return efd;
}

int64_t
sys_memory(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	switch (nr) {
	case SYS_BRK:
		return do_brk(proc, a0);
	case SYS_MMAP:
		return do_mmap(proc, a0, a1, a2, a3, a4, a5);
	case SYS_MUNMAP:
		return do_munmap(proc, a0, a1);
	case SYS_MPROTECT:
		return do_mprotect(proc, a0, a1, a2);
	case SYS_MREMAP:
		return do_mremap(proc, a0, a1, a2, a3);
	case SYS_MADVISE:
		return 0;
	case SYS_MSYNC:
		return 0;
	case SYS_MEMFD_CREATE:
		return do_memfd_create(proc, a0, a1);
	case SYS_MLOCK:
	case SYS_MUNLOCK:
		(void)a0; (void)a1;
		return 0;
	case SYS_MLOCKALL:
	case SYS_MUNLOCKALL:
		return 0;
	case SYS_MINCORE: {
		/*
		 * Write all-present (1) to the vec array.
		 * Number of pages = ceil(len / PAGE_SIZE).
		 */
		uint64_t	len, npages, i;

		len = a1;
		npages = (len + 4095) / 4096;
		for (i = 0; i < npages; i++) {
			if (mem_write8(proc->mem, a2 + i, 1) != 0)
				return -LINUX_EFAULT;
		}
		return 0;
	}
	default:
		LOG_WARN("sys_memory: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
