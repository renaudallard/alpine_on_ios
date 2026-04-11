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

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <limits.h>

#include <string.h>
#include <unistd.h>

#include "elf_loader.h"
#include "emu.h"
#include "memory.h"
#include "log.h"

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

/* ELF64 types defined inline to avoid host elf.h dependency. */

#define EI_NIDENT	16

#define ELFMAG0		0x7f
#define ELFMAG1		'E'
#define ELFMAG2		'L'
#define ELFMAG3		'F'

#define ELFCLASS64	2
#define ELFDATA2LSB	1

#define ET_EXEC		2
#define ET_DYN		3

#define EM_AARCH64	183

#define PT_NULL		0
#define PT_LOAD		1
#define PT_INTERP	3
#define PT_PHDR		6

#define PF_X		1
#define PF_W		2
#define PF_R		4

/* Auxiliary vector types */
#define AT_NULL		0
#define AT_PHDR		3
#define AT_PHENT	4
#define AT_PHNUM	5
#define AT_PAGESZ	6
#define AT_BASE		7
#define AT_FLAGS	8
#define AT_ENTRY	9
#define AT_UID		11
#define AT_EUID		12
#define AT_GID		13
#define AT_EGID		14
#define AT_PLATFORM	15
#define AT_HWCAP	16
#define AT_RANDOM	25

#define PAGE_SIZE	4096

typedef struct {
	uint8_t		e_ident[EI_NIDENT];
	uint16_t	e_type;
	uint16_t	e_machine;
	uint32_t	e_version;
	uint64_t	e_entry;
	uint64_t	e_phoff;
	uint64_t	e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize;
	uint16_t	e_phentsize;
	uint16_t	e_phnum;
	uint16_t	e_shentsize;
	uint16_t	e_shnum;
	uint16_t	e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t	p_type;
	uint32_t	p_flags;
	uint64_t	p_offset;
	uint64_t	p_vaddr;
	uint64_t	p_paddr;
	uint64_t	p_filesz;
	uint64_t	p_memsz;
	uint64_t	p_align;
} Elf64_Phdr;

static int
elf_pflags_to_prot(uint32_t pflags)
{
	int	prot;

	prot = 0;
	if (pflags & PF_R)
		prot |= MEM_PROT_READ;
	if (pflags & PF_W)
		prot |= MEM_PROT_WRITE;
	if (pflags & PF_X)
		prot |= MEM_PROT_EXEC;
	return prot;
}

int
elf_load(const char *host_path, mem_space_t *mem, uint64_t base_hint,
    elf_info_t *info)
{
	Elf64_Ehdr	 ehdr;
	Elf64_Phdr	*phdrs;
	int		 fd;
	ssize_t		 n;
	uint64_t	 base, vmin, vmax;
	int		 i, is_dyn;

	phdrs = NULL;
	memset(info, 0, sizeof(*info));

	fd = open(host_path, O_RDONLY);
	if (fd < 0) {
		emu_set_error("elf: cannot open %s", host_path);
		return -1;
	}

	n = read(fd, &ehdr, sizeof(ehdr));
	if (n != sizeof(ehdr)) {
		emu_set_error("elf: short read on ELF header");
		goto fail;
	}

	/* Validate ELF magic. */
	if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
	    ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
		emu_set_error("elf: bad ELF magic");
		goto fail;
	}

	if (ehdr.e_ident[4] != ELFCLASS64) {
		emu_set_error("elf: not 64-bit ELF");
		goto fail;
	}

	if (ehdr.e_ident[5] != ELFDATA2LSB) {
		emu_set_error("elf: not little-endian");
		goto fail;
	}

	if (ehdr.e_machine != EM_AARCH64) {
		emu_set_error("elf: not aarch64 (machine=%d)", ehdr.e_machine);
		goto fail;
	}

	if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
		emu_set_error("elf: unsupported ELF type %d", ehdr.e_type);
		goto fail;
	}

	is_dyn = (ehdr.e_type == ET_DYN);

	/* Read program headers. */
	phdrs = calloc(ehdr.e_phnum, sizeof(Elf64_Phdr));
	if (phdrs == NULL)
		goto fail;

	if (lseek(fd, ehdr.e_phoff, SEEK_SET) < 0)
		goto fail;

	n = read(fd, phdrs, (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr));
	if (n != (ssize_t)((size_t)ehdr.e_phnum * sizeof(Elf64_Phdr))) {
		emu_set_error("elf: short read on program headers");
		goto fail;
	}

	/* First pass: find address range of PT_LOAD segments. */
	vmin = UINT64_MAX;
	vmax = 0;
	for (i = 0; i < ehdr.e_phnum; i++) {
		uint64_t	seg_end;

		if (phdrs[i].p_type != PT_LOAD)
			continue;
		if (phdrs[i].p_vaddr < vmin)
			vmin = phdrs[i].p_vaddr;
		seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
		if (seg_end > vmax)
			vmax = seg_end;
	}

	if (vmin == UINT64_MAX) {
		emu_set_error("elf: no PT_LOAD segments");
		goto fail;
	}

	/* Compute base address. */
	if (mem->aot_mode && !is_dyn) {
		emu_set_error("elf: ET_EXEC not supported in AOT mode "
		    "(only PIE/ET_DYN)");
		goto fail;
	}

	if (is_dyn) {
		if (mem->aot_mode) {
			/*
			 * AOT: dlopen the companion Mach-O dylib.
			 * The build script converts each ELF to a dylib
			 * that preserves code+data segment layout.
			 * dlopen() loads it with executable code pages
			 * (signed by the app bundle).
			 *
			 * Follow symlinks first.  Busybox applets
			 * (/bin/ls etc.) are installed in the Documents
			 * overlay as symlinks pointing at the real
			 * busybox file inside the app bundle.  The ELF
			 * content is reachable by just opening the
			 * symlink, but the .dylib companion lives next
			 * to the target, not the link, so we need to
			 * append ".dylib" to the resolved path.
			 */
			char dylib_path[PATH_MAX];
			char real_host_path[PATH_MAX];
			char unique_path[PATH_MAX];
			const char *base_for_dylib;
			void *dl;
			static int dlopen_seq;

			if (realpath(host_path, real_host_path) != NULL)
				base_for_dylib = real_host_path;
			else
				base_for_dylib = host_path;
			snprintf(dylib_path, sizeof(dylib_path),
			    "%s.dylib", base_for_dylib);

			/*
			 * dlopen of the same path returns the existing
			 * handle (refcount++), sharing writable DATA
			 * pages between parent and child.  When the
			 * child's musl runs RELR self-relocation on the
			 * shared pages, it double-applies them and hangs.
			 * Copy the dylib to a unique temp path so each
			 * process gets independent data pages.
			 */
			{
				int seq = __sync_fetch_and_add(
				    &dlopen_seq, 1);
				snprintf(unique_path, sizeof(unique_path),
				    "%s.%d", dylib_path, seq);
				int sfd = open(dylib_path, O_RDONLY);
				int dfd = open(unique_path,
				    O_WRONLY | O_CREAT | O_TRUNC, 0755);
				if (sfd >= 0 && dfd >= 0) {
					char cpbuf[65536];
					ssize_t nr;
					while ((nr = read(sfd, cpbuf,
					    sizeof(cpbuf))) > 0)
						(void)write(dfd, cpbuf,
						    (size_t)nr);
				}
				if (sfd >= 0) close(sfd);
				if (dfd >= 0) close(dfd);
			}

			dl = dlopen(unique_path, RTLD_NOW | RTLD_LOCAL);
			unlink(unique_path);
			if (dl == NULL) {
				/*
				 * The file is a Mach-O dylib companion at
				 * this point, not an ELF.  Use a label that
				 * matches what the user actually loaded.
				 */
				emu_set_error("dlopen %s: %s",
				    dylib_path, dlerror());
				goto fail;
			}
			info->dl_handle = dl;

			/*
			 * Find where dyld loaded it.  The dylib's
			 * __TEXT vmaddr is 0, so the slide IS the
			 * load address.  Use dladdr on the handle
			 * to find the base.
			 */

			/* Get the Mach-O header address from dyld. */
			{
				/*
				 * Walk loaded images to find our dylib.
				 * The header address is the ASLR slide base.
				 */
				uint64_t img_addr = 0;

#ifdef __APPLE__
				extern uint32_t _dyld_image_count(void);
				extern const char *_dyld_get_image_name(uint32_t);
				extern const void *_dyld_get_image_header(uint32_t);
				uint32_t img_count;
				const char *img_name;

				img_count = _dyld_image_count();
				for (uint32_t j = img_count; j > 0; j--) {
					img_name = _dyld_get_image_name(j - 1);
					if (img_name != NULL &&
					    strstr(img_name, ".dylib") != NULL) {
						const char *bn1, *bn2;
						bn1 = strrchr(dylib_path, '/');
						bn2 = strrchr(img_name, '/');
						if (bn1 && bn2 &&
						    strcmp(bn1, bn2) == 0) {
							img_addr = (uint64_t)
							    _dyld_get_image_header(
							    j - 1);
							break;
						}
					}
				}
#endif
				if (img_addr == 0) {
					emu_set_error("elf: cannot find dylib "
					    "in loaded images");
					dlclose(dl);
					info->dl_handle = NULL;
					goto fail;
				}

				/*
				 * The converter (elf2macho) uses iOS arm64
				 * page size (16K) regardless of host.  Match
				 * its calculation: shift is one page when the
				 * ELF's text sits in page 0 (so the Mach-O
				 * header gets its own leading page), and zero
				 * when text is already at or past page 1.
				 * Keep in sync with scripts/elf2macho.c.
				 */
				{
					const uint64_t IOS_PAGE = 0x4000;
					uint64_t tvp = 0, shift;
					int k;
					for (k = 0; k < ehdr.e_phnum; k++) {
						if (phdrs[k].p_type == PT_LOAD &&
						    (phdrs[k].p_flags & PF_X)) {
							tvp = phdrs[k].p_vaddr &
							    ~(IOS_PAGE - 1);
							break;
						}
					}
					shift = (tvp == 0) ? IOS_PAGE : 0;
					base = img_addr + shift;
				}
			}

			LOG_INFO("elf_load: AOT dylib %s at 0x%llx",
			    dylib_path, (unsigned long long)base);

			/*
			 * Register each PT_LOAD segment with mem_space.
			 * ELF segments are not required to start on a page
			 * boundary; the kernel maps from
			 *   page_align_down(p_vaddr)
			 * for
			 *   ((p_vaddr & (page-1)) + p_memsz)
			 * bytes.  Mirror that here so the registered range
			 * actually covers every byte the guest can address
			 * inside the segment, and so mem_mmap_host (which
			 * insists on a page-aligned start) sees an address
			 * it can accept verbatim.
			 */
			for (i = 0; i < ehdr.e_phnum; i++) {
				uint64_t	saddr, ssize, got;
				uint64_t	pa_addr, pa_size, skew;
				int		sprot;

				if (phdrs[i].p_type != PT_LOAD)
					continue;

				saddr = base + phdrs[i].p_vaddr;
				ssize = phdrs[i].p_memsz;
				sprot = elf_pflags_to_prot(phdrs[i].p_flags);

				/* Round saddr DOWN to a 16K boundary, grow
				 * the size by the resulting skew and round
				 * the total UP to 16K. */
				pa_addr = saddr & ~(uint64_t)0x3FFF;
				skew = saddr - pa_addr;
				pa_size = ALIGN_UP(skew + ssize, 0x4000);

				got = mem_mmap_host(mem, pa_addr, pa_size,
				    sprot, (uint8_t *)pa_addr,
				    MEM_MAP_EXTERNAL);
				if (got == (uint64_t)-1 || got != pa_addr) {
					emu_set_error("elf: register seg %d "
					    "wanted 0x%lx got 0x%lx", i,
					    (unsigned long)pa_addr,
					    (unsigned long)got);
					goto fail;
				}
			}

			info->entry = base + ehdr.e_entry;
			info->base = base;
			info->phent = sizeof(Elf64_Phdr);
			info->phnum = ehdr.e_phnum;
			/*
			 * AT_PHDR: the Mach-O layout keeps the Mach-O
			 * header at file offset 0, so the original ELF
			 * phdrs do not live at base + e_phoff.  Ship a
			 * copy of the raw phdr bytes in elf_info and let
			 * elf_setup_stack push them onto the guest
			 * stack, then set info->phdr to that guest
			 * address.  musl's ld.so walks them from there
			 * to find PT_TLS, PT_DYNAMIC, etc.
			 */
			info->phdr = 0;
			info->phdr_size =
			    (uint64_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
			info->phdr_data = malloc((size_t)info->phdr_size);
			if (info->phdr_data == NULL) {
				emu_set_error("elf: alloc phdr_data");
				goto fail;
			}
			memcpy(info->phdr_data, phdrs,
			    (size_t)info->phdr_size);
			{
				info->brk = ALIGN_UP(base + vmax, 0x4000);
			}

			/* Check for interpreter. */
			for (i = 0; i < ehdr.e_phnum; i++) {
				if (phdrs[i].p_type == PT_INTERP &&
				    phdrs[i].p_filesz > 0 &&
				    phdrs[i].p_filesz < sizeof(info->interp)) {
					if (lseek(fd, phdrs[i].p_offset,
					    SEEK_SET) < 0) {
						emu_set_error("elf: lseek interp");
						goto fail;
					}
					n = read(fd, info->interp,
					    phdrs[i].p_filesz);
					if (n != (ssize_t)phdrs[i].p_filesz) {
						emu_set_error("elf: short read interp");
						goto fail;
					}
					info->interp[n] = '\0';
				}
			}

			free(phdrs);
			close(fd);
			return 0;
		} else {
			base = base_hint;
			if (base == 0)
				base = 0x400000;
		}
	} else {
		base = 0;
	}

	/* Set initial brk to end of loaded segments (page-aligned). */
	{
		long bpg = mem->aot_mode ? sysconf(_SC_PAGESIZE) : PAGE_SIZE;
		if (bpg <= 0) bpg = PAGE_SIZE;
		info->brk = (base + vmax + (uint64_t)bpg - 1) &
		    ~((uint64_t)bpg - 1);
	}

	/* Second pass: load segments. */
	for (i = 0; i < ehdr.e_phnum; i++) {
		uint64_t	addr, map_addr, map_size, file_off;
		uint64_t	page_off;
		int		prot;
		void		*p;

		if (phdrs[i].p_type != PT_LOAD)
			continue;

		addr = base + phdrs[i].p_vaddr;
		page_off = addr & (PAGE_SIZE - 1);
		map_addr = addr - page_off;
		map_size = phdrs[i].p_memsz + page_off;

		prot = elf_pflags_to_prot(phdrs[i].p_flags);

		/* AOT mode is handled above via dlopen; not reached here. */

		/* Non-AOT: map with write permission so we can fill data. */
		if (mem_mmap(mem, map_addr, map_size,
		    prot | MEM_PROT_WRITE,
		    MEM_MAP_PRIVATE | MEM_MAP_FIXED | MEM_MAP_ANONYMOUS,
		    -1, 0) == (uint64_t)-1) {
			emu_set_error("elf: mmap failed for segment %d", i);
			goto fail;
		}

		/* Read file content. */
		if (phdrs[i].p_filesz > 0) {
			file_off = phdrs[i].p_offset;
			p = mem_translate(mem, addr, phdrs[i].p_filesz,
			    MEM_PROT_WRITE);
			if (p == NULL) {
				emu_set_error("elf: translate failed");
				goto fail;
			}
			if (lseek(fd, file_off, SEEK_SET) < 0)
				goto fail;
			n = read(fd, p, phdrs[i].p_filesz);
			if (n < 0 || (size_t)n < phdrs[i].p_filesz) {
				emu_set_error("elf: read segment %d failed "
				    "(got %zd, want %lu)", i, n,
				    (unsigned long)phdrs[i].p_filesz);
				goto fail;
			}
			LOG_DBG("elf_load: seg %d: vaddr=0x%lx "
			    "file=0x%lx fsz=0x%lx msz=0x%lx "
			    "mapaddr=0x%lx -> host=%p",
			    i, (unsigned long)phdrs[i].p_vaddr,
			    (unsigned long)file_off,
			    (unsigned long)phdrs[i].p_filesz,
			    (unsigned long)phdrs[i].p_memsz,
			    (unsigned long)map_addr, p);
		}

		/* Set final protection (remove write if not in flags). */
		if (!(phdrs[i].p_flags & PF_W))
			mem_mprotect(mem, map_addr, map_size, prot);
	}

	/* Check for interpreter and PHDR. */
	for (i = 0; i < ehdr.e_phnum; i++) {
		if (phdrs[i].p_type == PT_INTERP) {
			if (phdrs[i].p_filesz >= sizeof(info->interp)) {
				emu_set_error("elf: interp path too long");
				goto fail;
			}
			if (lseek(fd, phdrs[i].p_offset, SEEK_SET) < 0)
				goto fail;
			n = read(fd, info->interp, phdrs[i].p_filesz);
			if (n < (ssize_t)phdrs[i].p_filesz) {
				emu_set_error("elf: read interp failed");
				goto fail;
			}
			info->interp[phdrs[i].p_filesz] = '\0';
		}
		if (phdrs[i].p_type == PT_PHDR) {
			info->phdr = base + phdrs[i].p_vaddr;
		}
	}

	/* If no PT_PHDR, compute phdr location from first LOAD. */
	if (info->phdr == 0 && ehdr.e_phoff != 0)
		info->phdr = base + ehdr.e_phoff;

	info->entry = base + ehdr.e_entry;
	info->phent = sizeof(Elf64_Phdr);
	info->phnum = ehdr.e_phnum;
	info->base = base;

	/* Verify DYNAMIC section if present */
	for (i = 0; i < ehdr.e_phnum; i++) {
		if (phdrs[i].p_type == 2 /* PT_DYNAMIC */) {
			uint64_t dyn_addr = base + phdrs[i].p_vaddr;
			uint64_t *dp = mem_translate(mem, dyn_addr, 16,
			    MEM_PROT_READ);
			if (dp != NULL) {
				LOG_DBG("elf_load: DYNAMIC at 0x%lx:"
				    " tag=0x%lx val=0x%lx",
				    (unsigned long)dyn_addr,
				    (unsigned long)dp[0],
				    (unsigned long)dp[1]);
			} else {
				emu_set_error("elf: cannot translate DYNAMIC "
				    "at 0x%lx", (unsigned long)dyn_addr);
			}
		}
	}

	free(phdrs);
	close(fd);

	/*
	 * Interpreter loading is handled by the caller (proc_execve)
	 * which performs VFS path resolution.  We just record the
	 * interpreter path in info->interp.
	 */

	LOG_INFO("elf_load: %s loaded at base=0x%llx entry=0x%llx",
	    host_path, (unsigned long long)info->base,
	    (unsigned long long)info->entry);

	return 0;

fail:
	free(phdrs);
	if (info != NULL && info->phdr_data != NULL) {
		free(info->phdr_data);
		info->phdr_data = NULL;
		info->phdr_size = 0;
	}
	close(fd);
	return -1;
}

/* Write a string to guest memory, return its guest address.  Returns
 * 0 on failure (which is never a valid stack address). */
static uint64_t
push_string(mem_space_t *mem, uint64_t *sp, const char *str)
{
	size_t	len;

	len = strlen(str) + 1;
	*sp -= len;
	if (mem_copy_to(mem, *sp, str, len) != 0)
		return 0;
	return *sp;
}

uint64_t
elf_setup_stack(mem_space_t *mem, elf_info_t *info,
    const char **argv, const char **envp, uint64_t stack_top)
{
	uint64_t	sp, stack_base;
	uint64_t	random_addr, platform_addr;
	uint64_t	*argv_addrs, *envp_addrs;
	int		argc, envc, i;
	uint8_t		randbuf[16];

	/*
	 * Allocate stack pages.  In AOT mode, proc_execve already
	 * allocated and registered the stack region via mmap(); the
	 * pages are live at host addresses matching the guest
	 * addresses, so a second mem_mmap with MEM_MAP_FIXED would
	 * collide.  In interpreter mode, we allocate fresh pages.
	 */
	if (!mem->aot_mode) {
		stack_base = stack_top - (8 * 1024 * 1024);
		if (mem_mmap(mem, stack_base, 8 * 1024 * 1024,
		    MEM_PROT_READ | MEM_PROT_WRITE,
		    MEM_MAP_PRIVATE | MEM_MAP_FIXED | MEM_MAP_ANONYMOUS,
		    -1, 0) == (uint64_t)-1) {
			LOG_ERR("elf_setup_stack: stack mmap failed");
			return 0;
		}
	}

	sp = stack_top;

	/* Count arguments and environment. */
	argc = 0;
	if (argv != NULL) {
		while (argv[argc] != NULL)
			argc++;
	}
	envc = 0;
	if (envp != NULL) {
		while (envp[envc] != NULL)
			envc++;
	}

	/* Push strings onto the stack first (high addresses). */
	/* Platform string. */
	platform_addr = push_string(mem, &sp, "aarch64");
	if (platform_addr == 0) {
		LOG_ERR("elf_setup_stack: push platform failed");
		return 0;
	}

	/* Random bytes (just use fixed pseudo-random for reproducibility). */
	for (i = 0; i < 16; i++)
		randbuf[i] = (uint8_t)(i * 17 + 53);
	sp -= 16;
	if (mem_copy_to(mem, sp, randbuf, 16) != 0) {
		LOG_ERR("elf_setup_stack: push randbuf failed");
		return 0;
	}
	random_addr = sp;

	/*
	 * Copy the raw program headers onto the guest stack and point
	 * AT_PHDR at them.  musl's ld.so walks these to find PT_TLS,
	 * PT_DYNAMIC, etc.  Align the region to 8 bytes.
	 *
	 * musl derives the load base from the phdrs via
	 *   base = AT_PHDR - PT_PHDR.p_vaddr
	 * and then resolves every other segment as base + p_vaddr.
	 * Our phdrs live on the stack, not at their original ELF
	 * offset, so rewrite the PT_PHDR entry's p_vaddr to
	 * (sp - info->base) so musl's base calculation yields the
	 * real info->base.  Segments keep their original p_vaddr
	 * (0-based, relative to the load base).
	 */
	if (info->phdr_data != NULL && info->phdr_size > 0) {
		Elf64_Phdr	*phc;
		uint64_t	 nph, k;

		sp -= info->phdr_size;
		sp &= ~(uint64_t)7;

		phc = info->phdr_data;
		nph = info->phdr_size / sizeof(Elf64_Phdr);
		for (k = 0; k < nph; k++) {
			if (phc[k].p_type == PT_PHDR) {
				phc[k].p_vaddr = sp - info->base;
				phc[k].p_paddr = phc[k].p_vaddr;
				break;
			}
		}

		if (mem_copy_to(mem, sp, info->phdr_data,
		    (size_t)info->phdr_size) != 0) {
			LOG_ERR("elf_setup_stack: push phdrs failed");
			return 0;
		}
		info->phdr = sp;
	}

	/* Environment strings. */
	envp_addrs = NULL;
	if (envc > 0) {
		envp_addrs = calloc(envc, sizeof(uint64_t));
		if (envp_addrs == NULL) {
			LOG_ERR("elf_setup_stack: alloc envp_addrs failed");
			return 0;
		}
	}
	for (i = envc - 1; i >= 0; i--) {
		envp_addrs[i] = push_string(mem, &sp, envp[i]);
		if (envp_addrs[i] == 0) {
			LOG_ERR("elf_setup_stack: push envp[%d] failed", i);
			free(envp_addrs);
			return 0;
		}
	}

	/* Argument strings. */
	argv_addrs = NULL;
	if (argc > 0) {
		argv_addrs = calloc(argc, sizeof(uint64_t));
		if (argv_addrs == NULL) {
			LOG_ERR("elf_setup_stack: alloc argv_addrs failed");
			free(envp_addrs);
			return 0;
		}
	}
	for (i = argc - 1; i >= 0; i--) {
		argv_addrs[i] = push_string(mem, &sp, argv[i]);
		if (argv_addrs[i] == 0) {
			LOG_ERR("elf_setup_stack: push argv[%d] failed", i);
			free(argv_addrs);
			free(envp_addrs);
			return 0;
		}
	}

	/* Align to 16 bytes. */
	sp &= ~(uint64_t)15;

	/*
	 * Build the stack layout (from high to low):
	 *   auxv entries (pairs terminated by AT_NULL)
	 *   NULL (envp terminator)
	 *   envp[envc-1] ... envp[0]
	 *   NULL (argv terminator)
	 *   argv[argc-1] ... argv[0]
	 *   argc
	 *
	 * We build bottom-up by first computing how much space we
	 * need, then placing entries.
	 */

	/*
	 * Count auxv entries. Each is 2 uint64_t. We have:
	 * AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE,
	 * AT_FLAGS, AT_ENTRY, AT_UID, AT_EUID, AT_GID, AT_EGID,
	 * AT_RANDOM, AT_HWCAP, AT_PLATFORM, AT_NULL = 15 entries
	 */
	int	auxc = 15;
	int	total_slots;

	total_slots = 1 +		/* argc */
	    argc + 1 +			/* argv pointers + NULL */
	    envc + 1 +			/* envp pointers + NULL */
	    auxc * 2;			/* auxv pairs */

	/* Ensure alignment: total slots should be even for 16-byte align. */
	if (total_slots % 2 != 0)
		total_slots++;

	sp -= total_slots * 8;
	sp &= ~(uint64_t)15;

	uint64_t	pos;
	int		failed = 0;

	pos = sp;

#define PUT64(val) do {						\
	if (mem_write64(mem, pos, (uint64_t)(val)) != 0)	\
		failed = 1;					\
	pos += 8;						\
} while (0)

	/* argc */
	PUT64(argc);

	/* argv pointers */
	for (i = 0; i < argc; i++)
		PUT64(argv_addrs[i]);
	PUT64(0);	/* NULL terminator */

	/* envp pointers */
	for (i = 0; i < envc; i++)
		PUT64(envp_addrs[i]);
	PUT64(0);	/* NULL terminator */

	/* Auxiliary vector */
#define AUXV(type, val) do {	\
	PUT64(type);		\
	PUT64(val);		\
} while (0)

	AUXV(AT_PHDR, info->phdr);
	AUXV(AT_PHENT, info->phent);
	AUXV(AT_PHNUM, info->phnum);
	AUXV(AT_PAGESZ, PAGE_SIZE);
	AUXV(AT_BASE, info->interp_base);
	AUXV(AT_FLAGS, 0);
	AUXV(AT_ENTRY, info->entry);
	AUXV(AT_UID, 0);
	AUXV(AT_EUID, 0);
	AUXV(AT_GID, 0);
	AUXV(AT_EGID, 0);
	AUXV(AT_RANDOM, random_addr);
	/*
	 * aarch64 HWCAP bits (linux/arch/arm64/include/uapi/asm/hwcap.h):
	 *   FP      (1<<0)   ASIMD    (1<<1)   EVTSTRM  (1<<2)
	 *   AES     (1<<3)   PMULL    (1<<4)   SHA1     (1<<5)
	 *   SHA2    (1<<6)   CRC32    (1<<7)   ATOMICS  (1<<8)
	 *   FPHP    (1<<9)   ASIMDHP  (1<<10)  CPUID    (1<<11)
	 *   ASIMDRDM(1<<12)  JSCVT    (1<<13)  FCMA     (1<<14)
	 *   LRCPC   (1<<15)  DCPOP    (1<<16)  SHA3     (1<<17)
	 *   SM3     (1<<18)  SM4      (1<<19)  ASIMDDP  (1<<20)
	 *   SHA512  (1<<21)
	 * Every Apple M1+ (and any aarch64 that can host iOS) has
	 * all of these, so advertise them so musl can enable its
	 * LSE atomics, SHA, AES and NEON dot-product fast paths.
	 */
	AUXV(AT_HWCAP, 0x3fffffULL);
	AUXV(AT_PLATFORM, platform_addr);
	AUXV(AT_NULL, 0);

#undef AUXV
#undef PUT64

	free(argv_addrs);
	free(envp_addrs);

	if (failed) {
		LOG_ERR("elf_setup_stack: stack write failed");
		return 0;
	}

	LOG_DBG("elf_setup_stack: sp=0x%llx argc=%d envc=%d",
	    (unsigned long long)sp, argc, envc);

	return sp;
}
