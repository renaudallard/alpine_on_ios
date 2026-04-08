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
 * AOT mode: pre-patched binaries in the rootfs are loaded via
 * dlopen() of their Mach-O dylib companions.
 */
int emu_set_aot_enabled(int on);
int emu_aot_enabled(void);

/*
 * Set a writable overlay directory. Files in the overlay take
 * precedence over the rootfs (for /etc, /tmp, /home, etc.).
 */
void emu_set_overlay(const char *overlay_path);

/*
 * Get the last error message (for diagnostics on iOS).
 */
const char *emu_last_error(void);
void emu_set_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/*
 * Get diagnostic info about execution mode.
 * Returns a static string describing AOT/native base status.
 */
const char *emu_mode_info(void);

/*
 * Breadcrumb trail: append a line to a persistent file so that
 * after a silent crash (no .ips generated, e.g. signal in a
 * background thread during early native execution) the next app
 * launch can display where the last run died.  Call
 * emu_set_breadcrumb_path() once at startup, then emu_breadcrumb()
 * from any C path whose progress you want to record.
 */
void emu_set_breadcrumb_path(const char *path);
void emu_breadcrumb(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
/*
 * Read all breadcrumbs from the previous run (if any) into a
 * static buffer and truncate the file so the next run starts
 * clean.  Returns the static buffer, or "" if no previous run.
 */
const char *emu_breadcrumbs_reset(void);

#endif /* EMU_H */
