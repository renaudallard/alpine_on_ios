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

#include <sys/mman.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <termios.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <util.h>	/* openpty */
#else
#include <pty.h>	/* openpty on Linux (needs -lutil) */
#endif


#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "emu.h"
#include "native.h"
#include "log.h"
#include "memory.h"
#include "process.h"
#include "signal_emu.h"
#include "syscall.h"
#include "vfs.h"

/* Global state */
static vfs_t		*g_vfs;
static int		 g_initialized;
static int		 g_aot_enabled;
static pthread_mutex_t	 g_lock = PTHREAD_MUTEX_INITIALIZER;
extern uint64_t		 g_native_base;
static char		 g_last_error[512];

static void
set_error(const char *fmt, ...)
{
	va_list	ap;
	va_start(ap, fmt);
	vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
	va_end(ap);
	LOG_ERR("%s", g_last_error);
}

void
emu_set_error(const char *fmt, ...)
{
	va_list	ap;
	va_start(ap, fmt);
	vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
	va_end(ap);
	LOG_ERR("%s", g_last_error);
}

const char *
emu_last_error(void)
{
	return (g_last_error);
}

int
emu_init(const char *rootfs_path)
{
	pthread_mutex_lock(&g_lock);
	if (g_initialized) {
		pthread_mutex_unlock(&g_lock);
		return (-1);
	}

	log_init(LOG_LVL_INFO);
	proc_table_init();
	futex_init();

	g_vfs = vfs_create(rootfs_path);
	if (g_vfs == NULL) {
		pthread_mutex_unlock(&g_lock);
		return (-1);
	}

	/*
	 * AOT native execution: dylib companions loaded via dlopen.
	 * Require native_available and successful native_init (SIGTRAP
	 * handler).  If either fails, emu_init fails - we never run in
	 * interpreter mode.
	 */
	g_native_base = 0;

	if (!native_available()) {
		set_error("AOT not available: not aarch64");
		vfs_destroy(g_vfs);
		g_vfs = NULL;
		pthread_mutex_unlock(&g_lock);
		return (-1);
	}
	if (native_init() != 0) {
		set_error("AOT not available: native_init failed");
		vfs_destroy(g_vfs);
		g_vfs = NULL;
		pthread_mutex_unlock(&g_lock);
		return (-1);
	}

	g_aot_enabled = 1;
	g_native_base = 1;	/* flag: AOT enabled */
	LOG_INFO("emu: AOT enabled");

	g_initialized = 1;
	pthread_mutex_unlock(&g_lock);
	return (0);
}

int
emu_spawn(const char *path, const char **argv, const char **envp, int *term_fd)
{
	emu_process_t	*proc;
	int		 master, slave;
	int		 ret;
	struct winsize	 ws;
	struct termios	 tio;

	g_last_error[0] = '\0';	/* Clear previous error */

	if (!g_initialized) {
		set_error("not initialized");
		return (-1);
	}

	/*
	 * Preferred: real host pty pair so the guest sees an actual
	 * terminal on fd 0/1/2 and the host tty driver handles
	 * canonical input, echo, signal processing and window-size
	 * tracking for free.  On iOS the sandbox blocks openpty()
	 * with EPERM, so we fall back to a plain socketpair and
	 * let the sys_file ioctl handler fake the tty responses
	 * (TIOCGWINSZ/TCGETS/...) based on the fd's isatty() state.
	 */
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = 24;
	ws.ws_col = 80;
	memset(&tio, 0, sizeof(tio));
	/* Sensible canonical/cooked defaults: echo + line buffering +
	 * SIG on Ctrl-C, NL->CRNL on output, 8-bit, 38400 baud. */
	tio.c_iflag = ICRNL | IXON;
	tio.c_oflag = OPOST | ONLCR;
	tio.c_cflag = CS8 | CREAD;
	tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL |
	    IEXTEN;
	tio.c_cc[VINTR]  = 003;   /* Ctrl-C */
	tio.c_cc[VQUIT]  = 034;   /* Ctrl-\ */
	tio.c_cc[VERASE] = 0177;  /* DEL */
	tio.c_cc[VKILL]  = 025;   /* Ctrl-U */
	tio.c_cc[VEOF]   = 004;   /* Ctrl-D */
	tio.c_cc[VSTART] = 021;   /* Ctrl-Q */
	tio.c_cc[VSTOP]  = 023;   /* Ctrl-S */
	tio.c_cc[VSUSP]  = 032;   /* Ctrl-Z */
	tio.c_cc[VMIN]   = 1;
	tio.c_cc[VTIME]  = 0;

	if (openpty(&master, &slave, NULL, &tio, &ws) < 0) {
		int	sp[2];
		int	saved = errno;

		LOG_INFO("emu_spawn: openpty failed (%s); "
		    "falling back to socketpair",
		    strerror(saved));
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
			set_error("openpty: %s; socketpair: %s",
			    strerror(saved), strerror(errno));
			return (-1);
		}
		master = sp[0];
		slave = sp[1];
	}

	proc = proc_create(NULL);
	if (proc == NULL) {
		set_error("proc_create failed");
		close(master);
		close(slave);
		return (-1);
	}

	proc->vfs = g_vfs;
	snprintf(proc->cwd, sizeof(proc->cwd), "/");

	/*
	 * Wire fds 0, 1, 2 to the slave side of the pty.  Dup three
	 * times so each guest fd has its own host fd entry and
	 * close()/dup2() semantics work independently.
	 */
	{
		int fd0, fd1, fd2;
		fd0 = dup(slave);
		fd1 = dup(slave);
		fd2 = dup(slave);
		close(slave);
		if (fd0 < 0 || fd1 < 0 || fd2 < 0) {
			set_error("dup: %s", strerror(errno));
			if (fd0 >= 0) close(fd0);
			if (fd1 >= 0) close(fd1);
			if (fd2 >= 0) close(fd2);
			close(master);
			proc_destroy(proc);
			return (-1);
		}
		proc->fds->fds[0].type = FD_TTY;
		proc->fds->fds[0].real_fd = fd0;
		proc->fds->fds[1].type = FD_TTY;
		proc->fds->fds[1].real_fd = fd1;
		proc->fds->fds[2].type = FD_TTY;
		proc->fds->fds[2].real_fd = fd2;
	}

	ret = proc_execve(proc, path, argv, envp);
	if (ret != 0) {
		/* Only set generic error if elf_load didn't set a specific one */
		if (g_last_error[0] == '\0')
			set_error("execve %s: error %d", path, ret);
		close(master);
		proc_destroy(proc);
		return (-1);
	}

	ret = pthread_create(&proc->host_thread, NULL, proc_run, proc);
	if (ret != 0) {
		set_error("pthread_create: %s", strerror(ret));
		close(master);
		proc_destroy(proc);
		return (-1);
	}
	pthread_detach(proc->host_thread);

	*term_fd = master;
	return (proc->pid);
}

int
emu_set_winsize(int pid, unsigned short rows, unsigned short cols)
{
	emu_process_t	*proc;
	struct winsize	 ws;

	sys_set_winsize(rows, cols);

	proc = proc_find(pid);
	if (proc == NULL)
		return (-1);

	/*
	 * Forward to the real host pty driver via fd 0's backing
	 * host fd.  The driver will then deliver SIGWINCH to the
	 * foreground process group on its own, but we still send
	 * our own EMU_SIGWINCH below for processes that rely on
	 * the emulator's signal path.
	 */
	if (proc->fds != NULL) {
		fd_entry_t	*fde = fd_get(proc->fds, 0);
		if (fde != NULL && fde->type == FD_TTY &&
		    fde->real_fd >= 0) {
			memset(&ws, 0, sizeof(ws));
			ws.ws_row = rows;
			ws.ws_col = cols;
			(void)ioctl(fde->real_fd, TIOCSWINSZ, &ws);
		}
	}

	sig_send(proc, EMU_SIGWINCH);
	return (0);
}

int
emu_kill(int pid, int sig)
{
	emu_process_t	*proc;

	proc = proc_find(pid);
	if (proc == NULL)
		return (-1);

	return sig_send(proc, sig);
}

int
emu_waitpid(int pid, int *status, int options)
{
	emu_process_t	*init;

	init = proc_find(1);
	if (init == NULL)
		return (-1);

	return proc_wait(init, pid, status, options);
}

void
emu_run(void)
{
	for (;;) {
		emu_process_t	*p;

		p = proc_find(1);
		if (p == NULL || p->state != PROC_RUNNING)
			break;
		usleep(100000);
	}
}

void
emu_shutdown(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_vfs != NULL) {
		vfs_destroy(g_vfs);
		g_vfs = NULL;
	}
	g_initialized = 0;
	pthread_mutex_unlock(&g_lock);
}

int
emu_set_aot_enabled(int on)
{
	int	prev;

	pthread_mutex_lock(&g_lock);
	prev = g_aot_enabled;
	g_aot_enabled = on;
	pthread_mutex_unlock(&g_lock);
	return (prev);
}

int
emu_aot_enabled(void)
{
	return (g_aot_enabled);
}

const char *
emu_mode_info(void)
{
	static char	buf[64];

	snprintf(buf, sizeof(buf), "aot=%d", g_aot_enabled);
	return (buf);
}

/*
 * ---- Breadcrumbs ------------------------------------------------
 *
 * Append-only progress log used to diagnose silent crashes where
 * iOS does not emit a standard .ips file (signal in a background
 * thread during early native execution, jetsam kill, or a clean
 * exit from a C fatal path).  The Swift bridge hands us a file
 * path under the app's Documents directory once at startup; we
 * keep the file open between calls and fflush() after each line
 * so whatever is on disk always reflects the last point we
 * reached, even if the next line would have been in the middle
 * of a crash.  emu_breadcrumbs_reset() drains the file at startup
 * and truncates it so each run starts clean.
 */
static char		 g_breadcrumb_path[1024];
static FILE		*g_breadcrumb_fp;
static pthread_mutex_t	 g_breadcrumb_lock = PTHREAD_MUTEX_INITIALIZER;

void
emu_set_breadcrumb_path(const char *path)
{
	pthread_mutex_lock(&g_breadcrumb_lock);
	if (g_breadcrumb_fp != NULL) {
		fclose(g_breadcrumb_fp);
		g_breadcrumb_fp = NULL;
	}
	if (path != NULL)
		snprintf(g_breadcrumb_path, sizeof(g_breadcrumb_path),
		    "%s", path);
	else
		g_breadcrumb_path[0] = '\0';
	pthread_mutex_unlock(&g_breadcrumb_lock);
}

void
emu_breadcrumb(const char *fmt, ...)
{
	char	line[512];
	va_list	ap;
	int	n;

	pthread_mutex_lock(&g_breadcrumb_lock);
	if (g_breadcrumb_path[0] == '\0') {
		pthread_mutex_unlock(&g_breadcrumb_lock);
		return;
	}
	if (g_breadcrumb_fp == NULL) {
		g_breadcrumb_fp = fopen(g_breadcrumb_path, "a");
		if (g_breadcrumb_fp == NULL) {
			pthread_mutex_unlock(&g_breadcrumb_lock);
			return;
		}
	}
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n] = '\n';
	line[n + 1] = '\0';
	fputs(line, g_breadcrumb_fp);
	fflush(g_breadcrumb_fp);
	pthread_mutex_unlock(&g_breadcrumb_lock);

	/* Also mirror to the regular log so we still get it via
	 * the normal stderr path when a Mac is attached. */
	LOG_INFO("bcrumb: %s", line);
}

const char *
emu_breadcrumbs_reset(void)
{
	static char	buf[16384];
	FILE		*f;
	size_t		n;

	buf[0] = '\0';
	pthread_mutex_lock(&g_breadcrumb_lock);
	if (g_breadcrumb_fp != NULL) {
		fclose(g_breadcrumb_fp);
		g_breadcrumb_fp = NULL;
	}
	if (g_breadcrumb_path[0] == '\0') {
		pthread_mutex_unlock(&g_breadcrumb_lock);
		return (buf);
	}
	f = fopen(g_breadcrumb_path, "r");
	if (f != NULL) {
		/*
		 * Read the tail of the file: if the previous run
		 * logged thousands of syscalls, only the last ~16k
		 * bytes fit in the UI buffer and are what we need
		 * to identify the hang point.
		 */
		if (fseek(f, 0, SEEK_END) == 0) {
			long end = ftell(f);
			long start = end > (long)(sizeof(buf) - 1)
			    ? end - (long)(sizeof(buf) - 1) : 0;
			fseek(f, start, SEEK_SET);
		}
		n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		fclose(f);
	}
	/* Truncate so the next run's crumbs don't pile on top. */
	f = fopen(g_breadcrumb_path, "w");
	if (f != NULL)
		fclose(f);
	pthread_mutex_unlock(&g_breadcrumb_lock);
	return (buf);
}

void
emu_set_overlay(const char *overlay_path)
{
	if (g_vfs != NULL)
		vfs_set_overlay(g_vfs, overlay_path);
}
