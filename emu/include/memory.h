/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* Protection flags (match Linux PROT_*) */
#define MEM_PROT_NONE	0
#define MEM_PROT_READ	1
#define MEM_PROT_WRITE	2
#define MEM_PROT_EXEC	4

/* Mapping flags (match Linux MAP_*) */
#define MEM_MAP_PRIVATE		0x02
#define MEM_MAP_SHARED		0x01
#define MEM_MAP_FIXED		0x10
#define MEM_MAP_ANONYMOUS	0x20

/* A contiguous region of guest memory */
typedef struct mem_region {
	uint64_t		base;	/* Guest virtual address */
	uint64_t		size;
	int			prot;	/* MEM_PROT_* */
	int			flags;	/* MEM_MAP_* */
	uint8_t			*host;	/* Host allocation */
	struct mem_region	*next;
} mem_region_t;

/* Virtual address space for one process */
typedef struct mem_space {
	mem_region_t	*regions;	/* Sorted by base addr */
	uint64_t	brk_base;	/* Heap start */
	uint64_t	brk_current;	/* Current brk */
	uint64_t	mmap_next;	/* Next mmap hint addr */
	pthread_mutex_t	lock;
} mem_space_t;

/* Lifecycle */
mem_space_t	*mem_space_create(void);
void		 mem_space_destroy(mem_space_t *);
mem_space_t	*mem_space_clone(mem_space_t *);

/* Memory mapping */
uint64_t	mem_mmap(mem_space_t *, uint64_t addr, uint64_t size,
		    int prot, int flags, int fd, uint64_t offset);
int		mem_munmap(mem_space_t *, uint64_t addr, uint64_t size);
int		mem_mprotect(mem_space_t *, uint64_t addr, uint64_t size,
		    int prot);

/* Heap management */
uint64_t	mem_brk(mem_space_t *, uint64_t addr);

/* Raw access (returns host pointer or NULL on fault) */
void		*mem_translate(mem_space_t *, uint64_t addr, uint64_t size,
		    int prot);

/* Typed read/write (return 0 on success, -1 on fault) */
int		mem_read8(mem_space_t *, uint64_t addr, uint8_t *val);
int		mem_read16(mem_space_t *, uint64_t addr, uint16_t *val);
int		mem_read32(mem_space_t *, uint64_t addr, uint32_t *val);
int		mem_read64(mem_space_t *, uint64_t addr, uint64_t *val);
int		mem_write8(mem_space_t *, uint64_t addr, uint8_t val);
int		mem_write16(mem_space_t *, uint64_t addr, uint16_t val);
int		mem_write32(mem_space_t *, uint64_t addr, uint32_t val);
int		mem_write64(mem_space_t *, uint64_t addr, uint64_t val);

/* Bulk copy */
int		mem_copy_to(mem_space_t *, uint64_t addr, const void *src,
		    uint64_t size);
int		mem_copy_from(mem_space_t *, void *dst, uint64_t addr,
		    uint64_t size);

/* Read a NUL-terminated string from guest memory */
int		mem_read_str(mem_space_t *, uint64_t addr, char *buf,
		    size_t bufsiz);

#endif /* MEMORY_H */
