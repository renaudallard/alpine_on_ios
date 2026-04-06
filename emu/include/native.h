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

#ifndef NATIVE_H
#define NATIVE_H

#include <stddef.h>

/* Forward declarations */
typedef struct emu_process emu_process_t;
typedef struct cpu_state cpu_state_t;

/*
 * Initialize native engine (installs signal handler).
 * Returns 0 on success, -1 on error.
 */
int	native_init(void);

/*
 * Check if native execution is available on this platform.
 * Returns 1 if available, 0 otherwise.
 */
int	native_available(void);

/*
 * Run a process using native native execution.
 * Returns the process exit code.
 */
int	native_run(emu_process_t *proc);

/*
 * Patch code for native execution: replace SVC #0, MSR/MRS TPIDR_EL0
 * with BRK instructions that the signal handler intercepts.
 */
void	native_patch_code(void *code, size_t size);

/*
 * W^X toggle for native code regions.
 * On Apple platforms, pthread_native_write_protect_np toggles MAP_JIT
 * page permissions. The function exists at runtime on iOS 14.2+ and
 * macOS 11+ but iOS SDK headers mark it __API_UNAVAILABLE(ios).
 * We resolve it via dlsym to bypass the availability check.
 * On Linux: RWX pages, no toggle needed.
 */
#if defined(__APPLE__) && defined(__MACH__)
void	native_write_protect(int enabled);
#define NATIVE_WRITE_ENABLE()	native_write_protect(0)
#define NATIVE_WRITE_DISABLE()	native_write_protect(1)
#else
#define NATIVE_WRITE_ENABLE()	do {} while (0)
#define NATIVE_WRITE_DISABLE()	do {} while (0)
#endif

#ifdef __aarch64__
/* Assembly stubs */
void	native_enter(cpu_state_t *cpu, void *host_pc);
void	native_exit(void);
#endif

#endif /* NATIVE_H */
