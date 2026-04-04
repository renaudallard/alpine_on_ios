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

#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stddef.h>

/* Return codes */
#define EMU_OK         0
#define EMU_ERR       -1
#define EMU_SYSCALL   -2
#define EMU_BREAK     -3
#define EMU_UNIMPL    -4
#define EMU_SEGFAULT  -5
#define EMU_EXIT      -6

/*
 * Initialize the emulator.
 * rootfs_path: path to the Alpine aarch64 rootfs directory.
 * Returns 0 on success, -1 on error.
 */
int emu_init(const char *rootfs_path);

/*
 * Spawn a new process from an ELF binary.
 * path: path inside the rootfs (e.g. "/bin/sh").
 * argv, envp: NULL-terminated argument and environment arrays.
 * term_fd: on success, set to a file descriptor for terminal I/O.
 * Returns the PID on success, -1 on error.
 */
int emu_spawn(const char *path, const char **argv, const char **envp,
    int *term_fd);

/*
 * Set terminal window size for a process.
 */
int emu_set_winsize(int pid, unsigned short rows, unsigned short cols);

/*
 * Send a signal to a process.
 */
int emu_kill(int pid, int sig);

/*
 * Wait for a process to change state.
 * Returns 0 on success, -1 on error.
 */
int emu_waitpid(int pid, int *status, int options);

/*
 * Run the emulator event loop. Blocks until all processes exit.
 * Must be called from a dedicated thread.
 */
void emu_run(void);

/*
 * Shut down the emulator and free all resources.
 */
void emu_shutdown(void);

/*
 * Enable or disable JIT native execution.
 * Returns the previous state.
 */
int emu_set_jit_enabled(int on);

/*
 * Check if JIT is enabled.
 */
int emu_jit_enabled(void);

/*
 * Get the last error message (for diagnostics on iOS).
 */
const char *emu_last_error(void);

#endif /* EMU_H */
