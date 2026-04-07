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
	uint64_t	phdr;		/* Guest address of program headers
					 * (filled in by elf_setup_stack, which
					 * copies phdr_data into the stack). */
	uint64_t	phent;		/* Size of one phdr entry */
	uint64_t	phnum;		/* Number of phdr entries */
	uint64_t	base;		/* Load base address (for PIE/interp) */
	uint64_t	brk;		/* Initial brk (end of loaded segments) */
	uint64_t	interp_base;	/* Interpreter load base (0 if static) */
	uint64_t	interp_entry;	/* Interpreter entry point */
	char		interp[256];	/* Interpreter path (empty if static) */
	void		*dl_handle;	/* dlopen handle (AOT mode), or NULL */

	/*
	 * Raw program-header bytes, owned by elf_info.  elf_load malloc's
	 * this; proc_execve frees it after elf_setup_stack has copied the
	 * data onto the guest stack.  Needed so musl's ld.so can walk
	 * PT_TLS and friends via AT_PHDR.
	 */
	void		*phdr_data;
	uint64_t	phdr_size;	/* phnum * phent */
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
 * Set up the initial process stack.  Writes info->phdr with the
 * guest address of the copied program headers (so AT_PHDR is valid),
 * which is why info is no longer const.
 * Returns the initial stack pointer value.
 */
uint64_t elf_setup_stack(mem_space_t *mem, elf_info_t *info,
	    const char **argv, const char **envp,
	    uint64_t stack_top);

#endif /* ELF_LOADER_H */
