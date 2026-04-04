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

#ifndef JIT_H
#define JIT_H

#include <stddef.h>

/* Forward declarations */
typedef struct emu_process emu_process_t;
typedef struct cpu_state cpu_state_t;

/*
 * Initialize JIT engine (installs signal handler).
 * Returns 0 on success, -1 on error.
 */
int	jit_init(void);

/*
 * Check if JIT execution is available on this platform.
 * Returns 1 if available, 0 otherwise.
 */
int	jit_available(void);

/*
 * Run a process using native JIT execution.
 * Returns the process exit code.
 */
int	jit_run(emu_process_t *proc);

/*
 * Patch code for JIT execution: replace SVC #0, MSR/MRS TPIDR_EL0
 * with BRK instructions that the signal handler intercepts.
 */
void	jit_patch_code(void *code, size_t size);

/*
 * W^X toggle for JIT code regions.
 * On Apple platforms (macOS 11+, iOS 14.2+):
 *   pthread_jit_write_protect_np(0) makes MAP_JIT pages writable.
 *   pthread_jit_write_protect_np(1) makes them executable.
 * On Linux: RWX pages, no toggle needed.
 */
#if defined(__APPLE__) && defined(__MACH__)
#include <pthread.h>
#define JIT_WRITE_ENABLE()	pthread_jit_write_protect_np(0)
#define JIT_WRITE_DISABLE()	pthread_jit_write_protect_np(1)
#else
#define JIT_WRITE_ENABLE()	do {} while (0)
#define JIT_WRITE_DISABLE()	do {} while (0)
#endif

#ifdef __aarch64__
/* Assembly stubs */
void	jit_enter(cpu_state_t *cpu, void *host_pc);
void	jit_exit(void);
#endif

#endif /* JIT_H */
