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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "vfs.h"
#include "log.h"

/* Linux errno */
#define LINUX_ENOENT	2
#define LINUX_EBADF	9
#define LINUX_ENOMEM	12
#define LINUX_EFAULT	14
#define LINUX_ENOTDIR	20
#define LINUX_EINVAL	22
#define LINUX_ERANGE	34
#define LINUX_ENOSYS	38

/* Linux futex operations */
#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_PRIVATE_FLAG	128

/* Linux clock IDs */
#define LINUX_CLOCK_REALTIME		0
#define LINUX_CLOCK_MONOTONIC		1
#define LINUX_CLOCK_PROCESS_CPUTIME_ID	2
#define LINUX_CLOCK_THREAD_CPUTIME_ID	3
#define LINUX_CLOCK_MONOTONIC_RAW	4
#define LINUX_CLOCK_REALTIME_COARSE	5
#define LINUX_CLOCK_MONOTONIC_COARSE	6
#define LINUX_CLOCK_BOOTTIME		7

/* UTS field size */
#define UTS_LEN		65

static int64_t
do_uname(emu_process_t *proc, uint64_t a0)
{
	/*
	 * struct utsname: 6 fields of 65 bytes each.
	 * sysname, nodename, release, version, machine, domainname
	 */
	char	utsname[6 * UTS_LEN];

	memset(utsname, 0, sizeof(utsname));
	strncpy(utsname + 0 * UTS_LEN, "Linux", UTS_LEN - 1);
	strncpy(utsname + 1 * UTS_LEN, "alpine", UTS_LEN - 1);
	strncpy(utsname + 2 * UTS_LEN, "6.1.0", UTS_LEN - 1);
	strncpy(utsname + 3 * UTS_LEN, "#1 SMP", UTS_LEN - 1);
	strncpy(utsname + 4 * UTS_LEN, "aarch64", UTS_LEN - 1);
	strncpy(utsname + 5 * UTS_LEN, "(none)", UTS_LEN - 1);

	if (mem_copy_to(proc->mem, a0, utsname, sizeof(utsname)) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_getcwd(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	size_t	len;

	len = strlen(proc->cwd) + 1;
	if (len > a1)
		return -LINUX_ERANGE;

	if (mem_copy_to(proc->mem, a0, proc->cwd, len) != 0)
		return -LINUX_EFAULT;
	return (int64_t)len;
}

static int64_t
do_chdir(emu_process_t *proc, uint64_t a0)
{
	char		guest_path[PATH_MAX];
	char		abs_path[PATH_MAX];
	char		host_path[PATH_MAX];
	struct stat	st;

	if (mem_read_str(proc->mem, a0, guest_path,
	    sizeof(guest_path)) != 0)
		return -LINUX_EFAULT;

	vfs_normalize_path(proc->cwd, guest_path, abs_path,
	    sizeof(abs_path));

	if (vfs_resolve(proc->vfs, abs_path, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (stat(host_path, &st) < 0)
		return -LINUX_ENOENT;

	if (!S_ISDIR(st.st_mode))
		return -LINUX_ENOTDIR;

	strncpy(proc->cwd, abs_path, sizeof(proc->cwd) - 1);
	proc->cwd[sizeof(proc->cwd) - 1] = '\0';

	return 0;
}

static int64_t
do_fchdir(emu_process_t *proc, uint64_t a0)
{
	/* Stub: difficult without tracking fd->path mappings. */
	(void)proc;
	(void)a0;
	return 0;
}

static int64_t
do_umask(emu_process_t *proc, uint64_t a0)
{
	uint32_t	old;

	old = proc->umask_val;
	proc->umask_val = (uint32_t)a0 & 0777;
	return old;
}

static int
translate_clockid(int linux_id)
{
	switch (linux_id) {
	case LINUX_CLOCK_REALTIME:
	case LINUX_CLOCK_REALTIME_COARSE:
		return CLOCK_REALTIME;
	case LINUX_CLOCK_MONOTONIC:
	case LINUX_CLOCK_MONOTONIC_RAW:
	case LINUX_CLOCK_MONOTONIC_COARSE:
	case LINUX_CLOCK_BOOTTIME:
		return CLOCK_MONOTONIC;
#ifdef CLOCK_PROCESS_CPUTIME_ID
	case LINUX_CLOCK_PROCESS_CPUTIME_ID:
		return CLOCK_PROCESS_CPUTIME_ID;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
	case LINUX_CLOCK_THREAD_CPUTIME_ID:
		return CLOCK_THREAD_CPUTIME_ID;
#endif
	default:
		return CLOCK_REALTIME;
	}
}

static int64_t
do_gettimeofday(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	struct timeval	tv;

	(void)a1;	/* timezone, deprecated */

	gettimeofday(&tv, NULL);

	if (a0 != 0) {
		int64_t	sec, usec;

		sec = (int64_t)tv.tv_sec;
		usec = (int64_t)tv.tv_usec;
		if (mem_write64(proc->mem, a0, (uint64_t)sec) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a0 + 8, (uint64_t)usec) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_clock_gettime(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	struct timespec	ts;
	int		clockid;

	clockid = translate_clockid((int)a0);
	if (clock_gettime(clockid, &ts) < 0)
		return -LINUX_EINVAL;

	if (a1 != 0) {
		int64_t	sec, nsec;

		sec = (int64_t)ts.tv_sec;
		nsec = (int64_t)ts.tv_nsec;
		if (mem_write64(proc->mem, a1, (uint64_t)sec) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a1 + 8, (uint64_t)nsec) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_clock_getres(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	(void)a0;

	if (a1 != 0) {
		/* Return 1 nanosecond resolution. */
		if (mem_write64(proc->mem, a1, 0) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a1 + 8, 1) != 0)
			return -LINUX_EFAULT;
	}
	return 0;
}

static int64_t
do_clock_nanosleep(emu_process_t *proc, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
	struct timespec	req, rem;
	int		clockid;

	(void)a1;	/* flags */

	clockid = translate_clockid((int)a0);
	(void)clockid;

	if (a2 != 0) {
		uint64_t	sec, nsec;

		if (mem_read64(proc->mem, a2, &sec) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a2 + 8, &nsec) != 0)
			return -LINUX_EFAULT;
		req.tv_sec = (time_t)sec;
		req.tv_nsec = (long)nsec;
	} else {
		return -LINUX_EINVAL;
	}

	nanosleep(&req, &rem);

	if (a3 != 0) {
		mem_write64(proc->mem, a3, (uint64_t)rem.tv_sec);
		mem_write64(proc->mem, a3 + 8, (uint64_t)rem.tv_nsec);
	}

	return 0;
}

static int64_t
do_nanosleep(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	struct timespec	req, rem;

	if (a0 != 0) {
		uint64_t	sec, nsec;

		if (mem_read64(proc->mem, a0, &sec) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a0 + 8, &nsec) != 0)
			return -LINUX_EFAULT;
		req.tv_sec = (time_t)sec;
		req.tv_nsec = (long)nsec;
	} else {
		return -LINUX_EINVAL;
	}

	nanosleep(&req, &rem);

	if (a1 != 0) {
		mem_write64(proc->mem, a1, (uint64_t)rem.tv_sec);
		mem_write64(proc->mem, a1 + 8, (uint64_t)rem.tv_nsec);
	}

	return 0;
}

static int64_t
do_sysinfo(emu_process_t *proc, uint64_t a0)
{
	/*
	 * struct sysinfo layout (aarch64):
	 * uptime(8), loads[3](24), totalram(8), freeram(8),
	 * sharedram(8), bufferram(8), totalswap(8), freeswap(8),
	 * procs(2), pad(6), totalhigh(8), freehigh(8), mem_unit(4)
	 */
	uint8_t	buf[112];

	memset(buf, 0, sizeof(buf));

	/* uptime = 60 seconds */
	uint64_t	uptime = 60;

	memcpy(buf, &uptime, 8);

	/* totalram = 256 MB */
	uint64_t	totalram = 256ULL * 1024 * 1024;

	memcpy(buf + 32, &totalram, 8);

	/* freeram = 128 MB */
	uint64_t	freeram = 128ULL * 1024 * 1024;

	memcpy(buf + 40, &freeram, 8);

	/* procs = 1 */
	uint16_t	procs = 1;

	memcpy(buf + 80, &procs, 2);

	/* mem_unit = 1 */
	uint32_t	mem_unit = 1;

	memcpy(buf + 104, &mem_unit, 4);

	if (mem_copy_to(proc->mem, a0, buf, sizeof(buf)) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_getrandom(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	void	*buf;
	int	 fd;
	ssize_t	 n;

	(void)a2;	/* flags */

	if (a1 == 0)
		return 0;

	buf = mem_translate(proc->mem, a0, a1, MEM_PROT_WRITE);
	if (buf == NULL)
		return -LINUX_EFAULT;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		/* Fallback: fill with pseudo-random data. */
		size_t	i;

		for (i = 0; i < a1; i++)
			((uint8_t *)buf)[i] = (uint8_t)(i * 37 + 97);
		return (int64_t)a1;
	}

	n = read(fd, buf, (size_t)a1);
	close(fd);

	if (n < 0)
		return -LINUX_EINVAL;
	return n;
}

static int64_t
do_futex(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
	int	op;

	(void)a3;
	(void)a4;
	(void)a5;

	op = (int)a1 & ~FUTEX_PRIVATE_FLAG;

	switch (op) {
	case FUTEX_WAIT: {
		uint32_t	curval;

		if (mem_read32(proc->mem, a0, &curval) != 0)
			return -LINUX_EFAULT;

		/* If value does not match expected, return EAGAIN. */
		if (curval != (uint32_t)a2)
			return -11;	/* EAGAIN */

		/*
		 * In a full implementation we would block here.
		 * For now, return immediately (spurious wakeup).
		 */
		return 0;
	}
	case FUTEX_WAKE:
		/* Return 0 (no waiters woken in stub). */
		return 0;
	default:
		LOG_DBG("futex: unhandled op %d", op);
		return 0;
	}
}

int64_t
sys_misc(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	switch (nr) {
	case SYS_UNAME:
		return do_uname(proc, a0);
	case SYS_GETCWD:
		return do_getcwd(proc, a0, a1);
	case SYS_CHDIR:
		return do_chdir(proc, a0);
	case SYS_FCHDIR:
		return do_fchdir(proc, a0);
	case SYS_CHROOT:
		/* Stub: silently succeed. */
		return 0;
	case SYS_UMASK:
		return do_umask(proc, a0);
	case SYS_GETTIMEOFDAY:
		return do_gettimeofday(proc, a0, a1);
	case SYS_CLOCK_GETTIME:
		return do_clock_gettime(proc, a0, a1);
	case SYS_CLOCK_GETRES:
		return do_clock_getres(proc, a0, a1);
	case SYS_CLOCK_NANOSLEEP:
		return do_clock_nanosleep(proc, a0, a1, a2, a3);
	case SYS_NANOSLEEP:
		return do_nanosleep(proc, a0, a1);
	case SYS_GETITIMER:
		/* Stub: return zeroed timer. */
		if (a1 != 0) {
			uint8_t	buf[32];

			memset(buf, 0, sizeof(buf));
			mem_copy_to(proc->mem, a1, buf, sizeof(buf));
		}
		return 0;
	case SYS_SETITIMER:
		/* Stub: silently succeed, return old value if requested. */
		if (a2 != 0) {
			uint8_t	buf[32];

			memset(buf, 0, sizeof(buf));
			mem_copy_to(proc->mem, a2, buf, sizeof(buf));
		}
		return 0;
	case SYS_SYSINFO:
		return do_sysinfo(proc, a0);
	case SYS_GETRANDOM:
		return do_getrandom(proc, a0, a1, a2);
	case SYS_FUTEX:
		return do_futex(proc, a0, a1, a2, a3, a4, a5);
	case SYS_SYSLOG:
		return 0;
	default:
		LOG_WARN("sys_misc: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
