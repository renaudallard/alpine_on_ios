/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>

typedef struct cpu_state cpu_state_t;
typedef struct mem_space mem_space_t;

/* Information returned after loading an ELF */
typedef struct elf_info {
	uint64_t	entry;		/* Entry point */
	uint64_t	phdr;		/* Address of program headers in memory */
	uint64_t	phent;		/* Size of one phdr entry */
	uint64_t	phnum;		/* Number of phdr entries */
	uint64_t	base;		/* Load base address (for PIE/interp) */
	uint64_t	brk;		/* Initial brk (end of loaded segments) */
	uint64_t	interp_base;	/* Interpreter load base (0 if static) */
	uint64_t	interp_entry;	/* Interpreter entry point */
	char		interp[256];	/* Interpreter path (empty if static) */
} elf_info_t;

/*
 * Load an ELF64 aarch64 binary into memory.
 * host_path: path on the host filesystem.
 * mem: target memory space.
 * base_hint: base address hint (0 for default).
 * info: filled on success.
 * Returns 0 on success, -1 on error.
 */
int	elf_load(const char *host_path, mem_space_t *mem,
	    uint64_t base_hint, elf_info_t *info);

/*
 * Set up the initial process stack.
 * Returns the initial stack pointer value.
 */
uint64_t elf_setup_stack(mem_space_t *mem, const elf_info_t *info,
	    const char **argv, const char **envp,
	    uint64_t stack_top);

#endif /* ELF_LOADER_H */
