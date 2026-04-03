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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory.h"
#include "log.h"

#define PAGE_SIZE	4096
#define PAGE_MASK	(~((uint64_t)PAGE_SIZE - 1))
#define MMAP_START	0x7F0000000000ULL

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
	pthread_mutex_init(&ms->lock, NULL);
	return ms;
}

void
mem_space_destroy(mem_space_t *ms)
{
	mem_region_t	*r, *next;

	if (ms == NULL)
		return;

	for (r = ms->regions; r != NULL; r = next) {
		next = r->next;
		free(r->host);
		free(r);
	}

	pthread_mutex_destroy(&ms->lock);
	free(ms);
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

	pthread_mutex_lock(&src->lock);

	dst->brk_base = src->brk_base;
	dst->brk_current = src->brk_current;
	dst->mmap_next = src->mmap_next;
	pthread_mutex_init(&dst->lock, NULL);

	pp = &dst->regions;
	for (r = src->regions; r != NULL; r = r->next) {
		nr = calloc(1, sizeof(*nr));
		if (nr == NULL)
			goto fail;

		nr->base = r->base;
		nr->size = r->size;
		nr->prot = r->prot;
		nr->flags = r->flags;
		nr->host = calloc(1, r->size);
		if (nr->host == NULL) {
			free(nr);
			goto fail;
		}
		memcpy(nr->host, r->host, r->size);
		nr->next = NULL;
		*pp = nr;
		pp = &nr->next;
	}

	pthread_mutex_unlock(&src->lock);
	return dst;

fail:
	pthread_mutex_unlock(&src->lock);
	mem_space_destroy(dst);
	return NULL;
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
			free(r->host);
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
			tail->host = calloc(1, tail_size);
			if (tail->host == NULL) {
				free(tail);
				continue;
			}
			memcpy(tail->host, r->host + tail_off, tail_size);
			region_insert(ms, tail);

			/* Truncate original. */
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
			memmove(r->host, r->host + trim, r->size - trim);
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

	pthread_mutex_lock(&ms->lock);

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
		pthread_mutex_unlock(&ms->lock);
		return (uint64_t)-1;
	}

	r->host = calloc(1, aligned_size);
	if (r->host == NULL) {
		free(r);
		pthread_mutex_unlock(&ms->lock);
		return (uint64_t)-1;
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
	pthread_mutex_unlock(&ms->lock);

	LOG_TRACE("mmap: addr=0x%llx size=0x%llx prot=%d flags=0x%x",
	    (unsigned long long)addr, (unsigned long long)aligned_size,
	    prot, flags);

	return addr;
}

int
mem_munmap(mem_space_t *ms, uint64_t addr, uint64_t size)
{
	uint64_t	aligned_size;

	addr = page_align_down(addr);
	aligned_size = page_align_up(size);

	pthread_mutex_lock(&ms->lock);
	unmap_range(ms, addr, aligned_size);
	pthread_mutex_unlock(&ms->lock);

	return 0;
}

int
mem_mprotect(mem_space_t *ms, uint64_t addr, uint64_t size, int prot)
{
	mem_region_t	*r;
	uint64_t	 end;

	addr = page_align_down(addr);
	size = page_align_up(size);
	end = addr + size;

	pthread_mutex_lock(&ms->lock);
	for (r = ms->regions; r != NULL; r = r->next) {
		uint64_t	rend;

		rend = r->base + r->size;
		if (rend <= addr || r->base >= end)
			continue;
		r->prot = prot;
	}
	pthread_mutex_unlock(&ms->lock);

	return 0;
}

uint64_t
mem_brk(mem_space_t *ms, uint64_t addr)
{
	uint64_t	old_brk, new_brk, new_size;

	pthread_mutex_lock(&ms->lock);

	if (addr == 0) {
		pthread_mutex_unlock(&ms->lock);
		return ms->brk_current;
	}

	if (addr < ms->brk_base) {
		pthread_mutex_unlock(&ms->lock);
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

			if (r != NULL) {
				uint8_t	*newhost;

				newhost = realloc(r->host, r->size + grow);
				if (newhost == NULL) {
					pthread_mutex_unlock(&ms->lock);
					return ms->brk_current;
				}
				memset(newhost + r->size, 0, grow);
				r->host = newhost;
				r->size += grow;
			} else {
				r = calloc(1, sizeof(*r));
				if (r == NULL) {
					pthread_mutex_unlock(&ms->lock);
					return ms->brk_current;
				}
				new_size = new_brk - ms->brk_base;
				r->host = calloc(1, new_size);
				if (r->host == NULL) {
					free(r);
					pthread_mutex_unlock(&ms->lock);
					return ms->brk_current;
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
	pthread_mutex_unlock(&ms->lock);

	return addr;
}

void *
mem_translate(mem_space_t *ms, uint64_t addr, uint64_t size, int prot)
{
	mem_region_t	*r;

	pthread_mutex_lock(&ms->lock);
	for (r = ms->regions; r != NULL; r = r->next) {
		if (addr >= r->base && addr + size <= r->base + r->size) {
			if ((r->prot & prot) != prot) {
				pthread_mutex_unlock(&ms->lock);
				return NULL;
			}
			pthread_mutex_unlock(&ms->lock);
			return r->host + (addr - r->base);
		}
	}
	pthread_mutex_unlock(&ms->lock);
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
