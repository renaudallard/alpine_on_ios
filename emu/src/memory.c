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

#include <sys/mman.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <pthread.h>
#endif

#include "memory.h"
#include "native.h"
#include "log.h"

#define PAGE_SIZE	4096
#define PAGE_MASK	(~((uint64_t)PAGE_SIZE - 1))
#define MMAP_START	0x7F0000000000ULL

/* True when guest addr == host addr (AOT native mode). */
#define NATIVE_MODE(ms)	((ms)->aot_mode)

/* Host page size (may differ from guest 4K on macOS 16K). */
static uint64_t
host_page_size(void)
{
	static uint64_t	cached;

	if (cached == 0) {
		long sz = sysconf(_SC_PAGESIZE);
		cached = (sz > 0) ? (uint64_t)sz : PAGE_SIZE;
	}
	return cached;
}

static uint64_t
page_align_down(uint64_t addr)
{
	return addr & PAGE_MASK;
}

static uint64_t
page_align_up(uint64_t addr)
{
	return (addr + PAGE_SIZE - 1) & PAGE_MASK;
}

/* Insert region into sorted list. */
static void
region_insert(mem_space_t *ms, mem_region_t *r)
{
	mem_region_t	**pp;

	for (pp = &ms->regions; *pp != NULL; pp = &(*pp)->next) {
		if ((*pp)->base > r->base)
			break;
	}
	r->next = *pp;
	*pp = r;
}

/* Remove region from list. */
static void
region_remove(mem_space_t *ms, mem_region_t *r)
{
	mem_region_t	**pp;

	for (pp = &ms->regions; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == r) {
			*pp = r->next;
			return;
		}
	}
}

mem_space_t *
mem_space_create(void)
{
	mem_space_t	*ms;

	ms = calloc(1, sizeof(*ms));
	if (ms == NULL)
		return NULL;

	ms->mmap_next = MMAP_START;
	ms->refcount = 1;
	pthread_rwlock_init(&ms->lock, NULL);
	return ms;
}

void
mem_space_destroy(mem_space_t *ms)
{
	mem_region_t	*r, *next;
	int		 rc;

	if (ms == NULL)
		return;

	pthread_rwlock_wrlock(&ms->lock);
	rc = --ms->refcount;
	if (rc > 0) {
		pthread_rwlock_unlock(&ms->lock);
		return;
	}

	/* Last reference. Free regions while lock is held. */
	for (r = ms->regions; r != NULL; r = next) {
		next = r->next;
		if (r->flags & MEM_MAP_EXTERNAL)
			;	/* Not owned by us. */
		else if (r->flags & MEM_MAP_CALLOC)
			free(r->host);
		else if (NATIVE_MODE(ms))
			munmap(r->host, r->size);
		else
			free(r->host);
		free(r);
	}
	ms->regions = NULL;

	pthread_rwlock_unlock(&ms->lock);
	pthread_rwlock_destroy(&ms->lock);
	free(ms);
}

void
mem_space_ref(mem_space_t *ms)
{
	if (ms == NULL)
		return;

	pthread_rwlock_wrlock(&ms->lock);
	ms->refcount++;
	pthread_rwlock_unlock(&ms->lock);
}

mem_space_t *
mem_space_clone(mem_space_t *src)
{
	mem_space_t	*dst;
	mem_region_t	*r, *nr, **pp;

	if (src == NULL)
		return NULL;

	dst = calloc(1, sizeof(*dst));
	if (dst == NULL)
		return NULL;

	pthread_rwlock_rdlock(&src->lock);

	dst->brk_base = src->brk_base;
	dst->brk_current = src->brk_current;
	dst->refcount = 1;
	pthread_rwlock_init(&dst->lock, NULL);

	/*
	 * AOT fork: share the parent's region list read-only.
	 * The child is expected to call execve immediately, which
	 * creates a fresh AOT address space.  We reference the
	 * parent's host memory via MEM_MAP_EXTERNAL so the child
	 * cleanup doesn't free it.
	 */
	dst->aot_mode = src->aot_mode;
	dst->mmap_next = src->mmap_next;

	pp = &dst->regions;
	for (r = src->regions; r != NULL; r = r->next) {
		nr = calloc(1, sizeof(*nr));
		if (nr == NULL)
			goto fail;

		nr->base = r->base;
		nr->size = r->size;
		nr->prot = r->prot;
		/* Mark as external so child doesn't free parent's memory. */
		nr->flags = r->flags | MEM_MAP_EXTERNAL;
		nr->host = r->host;

		nr->next = NULL;
		*pp = nr;
		pp = &nr->next;
	}

	pthread_rwlock_unlock(&src->lock);
	return dst;

fail:
	pthread_rwlock_unlock(&src->lock);
	mem_space_destroy(dst);
	return NULL;
}

/* Free a region's host memory depending on native mode. */
static void
region_free_host(mem_space_t *ms, mem_region_t *r)
{
	if (r->flags & MEM_MAP_EXTERNAL)
		return;	/* Not owned by us. */
	if (r->flags & MEM_MAP_CALLOC)
		free(r->host);
	else if (NATIVE_MODE(ms))
		munmap(r->host, r->size);
	else
		free(r->host);
}

/* Remove any regions overlapping [addr, addr+size). */
static void
unmap_range(mem_space_t *ms, uint64_t addr, uint64_t size)
{
	mem_region_t	*r, *next;
	uint64_t	 end, rend;

	end = addr + size;

	for (r = ms->regions; r != NULL; r = next) {
		next = r->next;
		rend = r->base + r->size;

		if (rend <= addr || r->base >= end)
			continue;

		if (r->base >= addr && rend <= end) {
			/* Entirely within range, remove. */
			region_remove(ms, r);
			region_free_host(ms, r);
			free(r);
			continue;
		}

		if (r->base < addr && rend > end) {
			/* Split: region spans both sides. */
			mem_region_t	*tail;
			uint64_t	 tail_off, tail_size;

			tail_off = end - r->base;
			tail_size = rend - end;

			tail = calloc(1, sizeof(*tail));
			if (tail == NULL)
				continue;
			tail->base = end;
			tail->size = tail_size;
			tail->prot = r->prot;
			tail->flags = r->flags;

			if (NATIVE_MODE(ms)) {
				int	mflags, mprot;

				mflags = MAP_PRIVATE | MAP_ANONYMOUS |
				    MAP_FIXED;
				mprot = PROT_READ | PROT_WRITE;
				tail->host = mmap((void *)end, tail_size,
				    mprot, mflags, -1, 0);
				if (tail->host == MAP_FAILED) {
					free(tail);
					continue;
				}
				memcpy(tail->host, r->host + tail_off,
				    tail_size);
			} else {
				tail->host = calloc(1, tail_size);
				if (tail->host == NULL) {
					free(tail);
					continue;
				}
				memcpy(tail->host, r->host + tail_off,
				    tail_size);
			}

			region_insert(ms, tail);

			/* Truncate original (native mode: partial munmap
			 * not needed, mmap MAP_FIXED above reclaims). */
			r->size = addr - r->base;
			continue;
		}

		if (r->base < addr) {
			/* Overlap at end of region. */
			r->size = addr - r->base;
		} else {
			/* Overlap at start of region. */
			uint64_t	trim;

			trim = end - r->base;
			if (!NATIVE_MODE(ms)) {
				uint8_t	*newhost;

				newhost = calloc(1, r->size - trim);
				if (newhost == NULL)
					continue;
				memcpy(newhost, r->host + trim,
				    r->size - trim);
				free(r->host);
				r->host = newhost;
			} else {
				r->host = r->host + trim;
			}
			r->base = end;
			r->size -= trim;
		}
	}
}

uint64_t
mem_mmap(mem_space_t *ms, uint64_t addr, uint64_t size, int prot,
    int flags, int fd, uint64_t offset)
{
	mem_region_t	*r;
	uint64_t	 aligned_size;

	if (size == 0)
		return (uint64_t)-1;

	aligned_size = page_align_up(size);

	pthread_rwlock_wrlock(&ms->lock);

	if (flags & MEM_MAP_FIXED) {
		addr = page_align_down(addr);
		unmap_range(ms, addr, aligned_size);
	} else {
		if (addr == 0)
			addr = ms->mmap_next;
		addr = page_align_up(addr);

		/*
		 * Find a gap. Walk regions and look for space
		 * starting from addr.
		 */
		for (;;) {
			mem_region_t	*cur;
			int		 conflict;

			conflict = 0;
			for (cur = ms->regions; cur != NULL; cur = cur->next) {
				uint64_t	ce;

				ce = cur->base + cur->size;
				if (addr < ce && addr + aligned_size > cur->base) {
					addr = page_align_up(ce);
					conflict = 1;
					break;
				}
			}
			if (!conflict)
				break;
		}
		if (addr + aligned_size > addr)
			ms->mmap_next = addr + aligned_size;
	}

	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		pthread_rwlock_unlock(&ms->lock);
		return (uint64_t)-1;
	}

	if (NATIVE_MODE(ms) && !(flags & MEM_MAP_FIXED)) {
		/*
		 * AOT mode, non-fixed: let the kernel choose a safe
		 * host address.  Set the guest address to match so
		 * native code can access it directly.
		 */
		int mprot = PROT_READ | PROT_WRITE;
		if (prot & MEM_PROT_EXEC)
			mprot |= PROT_EXEC;
		r->host = mmap(NULL, aligned_size, mprot,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (r->host == MAP_FAILED) {
			free(r);
			pthread_rwlock_unlock(&ms->lock);
			return (uint64_t)-1;
		}
		addr = (uint64_t)r->host;
		ms->mmap_next = addr + aligned_size;
	} else if (NATIVE_MODE(ms)) {
		/*
		 * AOT mode, MAP_FIXED: address is within a known
		 * reservation (ELF loader).  Safe to MAP_FIXED.
		 */
		r->host = mmap((void *)addr, aligned_size,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		    -1, 0);
		if (r->host == MAP_FAILED) {
			free(r);
			pthread_rwlock_unlock(&ms->lock);
			return (uint64_t)-1;
		}
	} else {
		r->host = calloc(1, aligned_size);
		if (r->host == NULL) {
			free(r);
			pthread_rwlock_unlock(&ms->lock);
			return (uint64_t)-1;
		}
	}
	r->base = addr;
	r->size = aligned_size;
	r->prot = prot;
	r->flags = flags;

	/* Read file content if backed by fd. */
	if (fd >= 0 && !(flags & MEM_MAP_ANONYMOUS)) {
		ssize_t	n;
		size_t	to_read;

		to_read = size;
		if (to_read > aligned_size)
			to_read = aligned_size;

		n = pread(fd, r->host, to_read, offset);
		if (n < 0)
			LOG_WARN("mmap: pread failed for fd %d", fd);
	}

	region_insert(ms, r);
	pthread_rwlock_unlock(&ms->lock);

	LOG_TRACE("mmap: addr=0x%llx size=0x%llx prot=%d flags=0x%x",
	    (unsigned long long)addr, (unsigned long long)aligned_size,
	    prot, flags);

	return addr;
}

/*
 * Map a caller-provided host buffer into the guest address space.
 * The buffer is NOT freed by munmap or mem_space_destroy.
 */
uint64_t
mem_mmap_host(mem_space_t *ms, uint64_t addr, uint64_t size,
    int prot, uint8_t *host_buf)
{
	mem_region_t	*r;
	uint64_t	 aligned_size;

	if (size == 0 || host_buf == NULL)
		return (uint64_t)-1;

	aligned_size = page_align_up(size);

	pthread_rwlock_wrlock(&ms->lock);

	if (addr == 0)
		addr = ms->mmap_next;
	addr = page_align_up(addr);

	/* Find a gap. */
	for (;;) {
		mem_region_t	*cur;
		int		 conflict;

		conflict = 0;
		for (cur = ms->regions; cur != NULL; cur = cur->next) {
			uint64_t	ce;

			ce = cur->base + cur->size;
			if (addr < ce && addr + aligned_size > cur->base) {
				addr = page_align_up(ce);
				conflict = 1;
				break;
			}
		}
		if (!conflict)
			break;
	}
	if (addr + aligned_size > addr)
		ms->mmap_next = addr + aligned_size;

	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		pthread_rwlock_unlock(&ms->lock);
		return (uint64_t)-1;
	}

	r->base = addr;
	r->size = aligned_size;
	r->prot = prot;
	r->flags = MEM_MAP_PRIVATE | MEM_MAP_EXTERNAL;
	r->host = host_buf;

	region_insert(ms, r);
	pthread_rwlock_unlock(&ms->lock);

	return addr;
}

/*
 * Map a file region into the guest address space at a fixed address.
 * Used by Linux unit tests only; on iOS, AOT uses dlopen() of Mach-O
 * dylibs instead.
 */
uint64_t
mem_mmap_file(mem_space_t *ms, uint64_t addr, uint64_t size,
    int prot, int fd, uint64_t offset)
{
	mem_region_t	*r;
	uint64_t	 aligned_size;
	int		 host_prot;
	int		 used_calloc;
	void		*p;

	if (size == 0)
		return (uint64_t)-1;

	used_calloc = 0;

	{
		long	 host_page;
		uint64_t host_mask;

		host_page = sysconf(_SC_PAGESIZE);
		if (host_page <= 0)
			host_page = PAGE_SIZE;
		host_mask = ~((uint64_t)host_page - 1);

		aligned_size = (size + (uint64_t)host_page - 1) & host_mask;
		addr = addr & host_mask;
		offset = offset & host_mask;
	}

	host_prot = 0;
	if (prot & MEM_PROT_READ)
		host_prot |= PROT_READ;
	if (prot & MEM_PROT_WRITE)
		host_prot |= PROT_WRITE;
	if (prot & MEM_PROT_EXEC)
		host_prot |= PROT_EXEC;

	/* File-backed mmap. Uses MAP_FIXED within a pre-reserved
	 * region (AOT mode) or at a chosen address (non-AOT). */
	p = mmap((void *)addr, aligned_size, host_prot,
	    MAP_PRIVATE | MAP_FIXED, fd, (off_t)offset);

	if (p == MAP_FAILED && NATIVE_MODE(ms)) {
		/*
		 * AOT fallback: kernel picks address, read content.
		 * Update addr so guest == host.
		 */
		p = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p != MAP_FAILED) {
			if (pread(fd, p, size, (off_t)offset) < 0) {
				munmap(p, aligned_size);
				p = MAP_FAILED;
			} else {
				addr = (uint64_t)p;
			}
		}
	}
	if (p == MAP_FAILED) {
		/* Calloc fallback (interpreter mode). */
		p = calloc(1, aligned_size);
		if (p == NULL) {
			LOG_ERR("mem_mmap_file: alloc failed");
			return (uint64_t)-1;
		}
		if (pread(fd, p, size, (off_t)offset) < 0) {
			LOG_ERR("mem_mmap_file: pread failed: %s",
			    strerror(errno));
			free(p);
			return (uint64_t)-1;
		}
		used_calloc = 1;
	}

	pthread_rwlock_wrlock(&ms->lock);

	unmap_range(ms, addr, aligned_size);

	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		if (used_calloc)
			free(p);
		else
			munmap(p, aligned_size);
		pthread_rwlock_unlock(&ms->lock);
		return (uint64_t)-1;
	}

	r->base = addr;
	r->size = aligned_size;
	r->prot = prot;
	r->flags = MEM_MAP_PRIVATE | (used_calloc ? MEM_MAP_CALLOC : 0);
	r->host = p;

	region_insert(ms, r);
	pthread_rwlock_unlock(&ms->lock);

	return addr;
}

int
mem_munmap(mem_space_t *ms, uint64_t addr, uint64_t size)
{
	uint64_t	aligned_size;

	addr = page_align_down(addr);
	aligned_size = page_align_up(size);

	pthread_rwlock_wrlock(&ms->lock);
	unmap_range(ms, addr, aligned_size);
	pthread_rwlock_unlock(&ms->lock);

	return 0;
}

int
mem_mprotect(mem_space_t *ms, uint64_t addr, uint64_t size, int prot)
{
	mem_region_t	*r, *next;
	uint64_t	 end;

	addr = page_align_down(addr);
	size = page_align_up(size);
	end = addr + size;

	pthread_rwlock_wrlock(&ms->lock);
	for (r = ms->regions; r != NULL; r = next) {
		uint64_t	rend, overlap_start, overlap_end;

		next = r->next;
		rend = r->base + r->size;
		if (rend <= addr || r->base >= end)
			continue;

		overlap_start = (addr > r->base) ? addr : r->base;
		overlap_end = (end < rend) ? end : rend;

		/*
		 * If the mprotect covers the entire region, just
		 * change the protection in place.
		 */
		if (overlap_start == r->base && overlap_end == rend) {
			r->prot = prot;
		} else {
			/*
			 * Partial overlap: split the region.
			 *
			 * In interpreter mode each split piece gets its
			 * own host allocation so free() is safe later.
			 * In native mode host==guest so we just adjust
			 * pointers (no separate alloc needed).
			 */
			if (overlap_start > r->base) {
				/* Split: keep [base, overlap_start) as-is,
				 * create [overlap_start, overlap_end) with new prot */
				mem_region_t *nr = calloc(1, sizeof(*nr));
				if (nr == NULL) break;
				nr->base = overlap_start;
				nr->size = overlap_end - overlap_start;
				nr->prot = prot;
				nr->flags = r->flags;

				if (!NATIVE_MODE(ms)) {
					nr->host = calloc(1, nr->size);
					if (nr->host == NULL) {
						free(nr);
						break;
					}
					memcpy(nr->host,
					    r->host + (overlap_start - r->base),
					    nr->size);
				} else {
					nr->host = r->host +
					    (overlap_start - r->base);
				}

				if (overlap_end < rend) {
					/* Also need a tail region */
					mem_region_t *tr = calloc(1, sizeof(*tr));
					if (tr == NULL) {
						if (!NATIVE_MODE(ms))
							free(nr->host);
						free(nr);
						break;
					}
					tr->base = overlap_end;
					tr->size = rend - overlap_end;
					tr->prot = r->prot;
					tr->flags = r->flags;

					if (!NATIVE_MODE(ms)) {
						tr->host = calloc(1, tr->size);
						if (tr->host == NULL) {
							free(nr->host);
							free(nr);
							free(tr);
							break;
						}
						memcpy(tr->host,
						    r->host +
						    (overlap_end - r->base),
						    tr->size);
					} else {
						tr->host = r->host +
						    (overlap_end - r->base);
					}
					tr->next = r->next;
					nr->next = tr;
				} else {
					nr->next = r->next;
				}

				if (!NATIVE_MODE(ms)) {
					/* Shrink original's host buffer. */
					uint64_t newsize;
					uint8_t *newhost;

					newsize = overlap_start - r->base;
					newhost = calloc(1, newsize);
					if (newhost != NULL) {
						memcpy(newhost, r->host,
						    newsize);
						free(r->host);
						r->host = newhost;
					}
				}
				r->size = overlap_start - r->base;
				r->next = nr;
				next = nr->next;
			} else {
				/* overlap_start == r->base, overlap_end < rend:
				 * change [base, overlap_end), keep [overlap_end, rend) */
				mem_region_t *nr = calloc(1, sizeof(*nr));
				if (nr == NULL) break;
				nr->base = overlap_end;
				nr->size = rend - overlap_end;
				nr->prot = r->prot;
				nr->flags = r->flags;

				if (!NATIVE_MODE(ms)) {
					nr->host = calloc(1, nr->size);
					if (nr->host == NULL) {
						free(nr);
						break;
					}
					memcpy(nr->host,
					    r->host + (overlap_end - r->base),
					    nr->size);
					/* Shrink original's host buffer. */
					uint64_t newsize;
					uint8_t *newhost;

					newsize = overlap_end - r->base;
					newhost = calloc(1, newsize);
					if (newhost != NULL) {
						memcpy(newhost, r->host,
						    newsize);
						free(r->host);
						r->host = newhost;
					}
				} else {
					nr->host = r->host +
					    (overlap_end - r->base);
				}

				nr->next = r->next;
				r->size = overlap_end - r->base;
				r->prot = prot;
				r->next = nr;
				next = nr->next;
			}
			continue;	/* native mprotect handled per-region below */
		}

		if (NATIVE_MODE(ms)) {
			int	hp = 0;
			if (prot & MEM_PROT_READ)
				hp |= PROT_READ;
			if (prot & MEM_PROT_WRITE)
				hp |= PROT_WRITE;
			if (prot & MEM_PROT_EXEC)
				hp |= PROT_EXEC;
			mprotect(r->host, r->size, hp);
		}
	}
	pthread_rwlock_unlock(&ms->lock);

	return 0;
}

uint64_t
mem_brk(mem_space_t *ms, uint64_t addr)
{
	uint64_t	old_brk, new_brk, new_size;

	pthread_rwlock_wrlock(&ms->lock);

	if (addr == 0) {
		pthread_rwlock_unlock(&ms->lock);
		return ms->brk_current;
	}

	if (addr < ms->brk_base) {
		pthread_rwlock_unlock(&ms->lock);
		return ms->brk_current;
	}

	old_brk = ms->brk_current;
	new_brk = page_align_up(addr);

	if (new_brk > old_brk) {
		/* Expand: allocate a region for the new pages. */
		uint64_t	 grow;
		mem_region_t	*r;

		grow = new_brk - page_align_up(old_brk);
		if (grow > 0) {
			/*
			 * Check if there is already a heap region we
			 * can extend, otherwise create one.
			 */
			for (r = ms->regions; r != NULL; r = r->next) {
				if (r->base + r->size == page_align_up(old_brk)
				    && r->base >= ms->brk_base)
					break;
			}

			if (r != NULL && !NATIVE_MODE(ms)) {
				uint8_t	*newhost;

				newhost = realloc(r->host, r->size + grow);
				if (newhost == NULL) {
					pthread_rwlock_unlock(&ms->lock);
					return ms->brk_current;
				}
				memset(newhost + r->size, 0, grow);
				r->host = newhost;
				r->size += grow;
			} else if (r != NULL && NATIVE_MODE(ms)) {
				/*
				 * Heap pre-allocated at execve time.  Just
				 * advance brk_current within the region.
				 */
				if (new_brk > r->base + r->size) {
					pthread_rwlock_unlock(&ms->lock);
					return ms->brk_current;
				}
				/* No actual work needed; brk_current moves. */
			} else {
				r = calloc(1, sizeof(*r));
				if (r == NULL) {
					pthread_rwlock_unlock(&ms->lock);
					return ms->brk_current;
				}
				new_size = new_brk - ms->brk_base;
				if (NATIVE_MODE(ms)) {
					/* Should not reach here in AOT -
					 * heap pre-allocated in execve. */
					free(r);
					pthread_rwlock_unlock(&ms->lock);
					return ms->brk_current;
				} else {
					r->host = calloc(1, new_size);
					if (r->host == NULL) {
						free(r);
						pthread_rwlock_unlock(
						    &ms->lock);
						return ms->brk_current;
					}
				}
				r->base = ms->brk_base;
				r->size = new_size;
				r->prot = MEM_PROT_READ | MEM_PROT_WRITE;
				r->flags = MEM_MAP_PRIVATE | MEM_MAP_ANONYMOUS;
				region_insert(ms, r);
			}
		}
	} else if (new_brk < page_align_up(old_brk)) {
		/* Shrink: trim or remove heap regions above new_brk. */
		unmap_range(ms, new_brk, page_align_up(old_brk) - new_brk);
	}

	ms->brk_current = addr;
	pthread_rwlock_unlock(&ms->lock);

	return addr;
}

void *
mem_translate(mem_space_t *ms, uint64_t addr, uint64_t size, int prot)
{
	mem_region_t	*r;

	/*
	 * Unified path for all modes. For native-mode regions,
	 * r->host == mmap'd address (== r->base), so the formula
	 * r->host + (addr - r->base) returns the same pointer.
	 * For interpreter regions, r->host is a calloc'd buffer.
	 * This allows mixing native and interpreter regions in
	 * AOT mode (needed when host page size > guest page size).
	 */
	pthread_rwlock_rdlock(&ms->lock);
	for (r = ms->regions; r != NULL; r = r->next) {
		if (addr >= r->base && addr + size <= r->base + r->size) {
			if ((r->prot & prot) != prot) {
				pthread_rwlock_unlock(&ms->lock);
				return NULL;
			}
			pthread_rwlock_unlock(&ms->lock);
			return r->host + (addr - r->base);
		}
	}
	pthread_rwlock_unlock(&ms->lock);
	return NULL;
}

int
mem_read8(mem_space_t *ms, uint64_t addr, uint8_t *val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 1, MEM_PROT_READ);
	if (p == NULL)
		return -1;
	*val = *p;
	return 0;
}

int
mem_read16(mem_space_t *ms, uint64_t addr, uint16_t *val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 2, MEM_PROT_READ);
	if (p == NULL)
		return -1;
	memcpy(val, p, 2);
	return 0;
}

int
mem_read32(mem_space_t *ms, uint64_t addr, uint32_t *val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 4, MEM_PROT_READ);
	if (p == NULL)
		return -1;
	memcpy(val, p, 4);
	return 0;
}

int
mem_read64(mem_space_t *ms, uint64_t addr, uint64_t *val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 8, MEM_PROT_READ);
	if (p == NULL)
		return -1;
	memcpy(val, p, 8);
	return 0;
}

int
mem_write8(mem_space_t *ms, uint64_t addr, uint8_t val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 1, MEM_PROT_WRITE);
	if (p == NULL)
		return -1;
	*p = val;
	return 0;
}

int
mem_write16(mem_space_t *ms, uint64_t addr, uint16_t val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 2, MEM_PROT_WRITE);
	if (p == NULL)
		return -1;
	memcpy(p, &val, 2);
	return 0;
}

int
mem_write32(mem_space_t *ms, uint64_t addr, uint32_t val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 4, MEM_PROT_WRITE);
	if (p == NULL)
		return -1;
	memcpy(p, &val, 4);
	return 0;
}

int
mem_write64(mem_space_t *ms, uint64_t addr, uint64_t val)
{
	uint8_t	*p;

	p = mem_translate(ms, addr, 8, MEM_PROT_WRITE);
	if (p == NULL)
		return -1;
	memcpy(p, &val, 8);
	return 0;
}

int
mem_copy_to(mem_space_t *ms, uint64_t addr, const void *src, uint64_t size)
{
	uint8_t	*p;

	if (size == 0)
		return 0;

	p = mem_translate(ms, addr, size, MEM_PROT_WRITE);
	if (p == NULL)
		return -1;
	memcpy(p, src, size);
	return 0;
}

int
mem_copy_from(mem_space_t *ms, void *dst, uint64_t addr, uint64_t size)
{
	uint8_t	*p;

	if (size == 0)
		return 0;

	p = mem_translate(ms, addr, size, MEM_PROT_READ);
	if (p == NULL)
		return -1;
	memcpy(dst, p, size);
	return 0;
}

int
mem_read_str(mem_space_t *ms, uint64_t addr, char *buf, size_t bufsiz)
{
	size_t	i;
	uint8_t	c;

	for (i = 0; i < bufsiz - 1; i++) {
		if (mem_read8(ms, addr + i, &c) != 0)
			return -1;
		buf[i] = (char)c;
		if (c == '\0')
			return 0;
	}
	buf[i] = '\0';
	return 0;
}
