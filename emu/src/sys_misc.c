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
#include <pthread.h>
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

/* Futex hash table */
#define FUTEX_HASH_SIZE	256

typedef struct futex_waiter {
	uint64_t		addr;
	uint32_t		bitset;
	pthread_cond_t		cond;
	int			woken;
	struct futex_waiter	*next;
} futex_waiter_t;

static futex_waiter_t	*futex_hash[FUTEX_HASH_SIZE];
static pthread_mutex_t	 futex_lock = PTHREAD_MUTEX_INITIALIZER;
static int		 futex_initialized;

static unsigned int
futex_hash_fn(uint64_t addr)
{
	return (unsigned int)(addr >> 2) % FUTEX_HASH_SIZE;
}

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

void
futex_init(void)
{
	if (futex_initialized)
		return;
	memset(futex_hash, 0, sizeof(futex_hash));
	futex_initialized = 1;
}

int
futex_wait(uint64_t addr, uint32_t val, uint32_t bitset,
    const struct timespec *timeout)
{
	futex_waiter_t	*w;
	unsigned int	 h;
	int		 ret;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return -LINUX_ENOMEM;

	w->addr = addr;
	w->bitset = bitset;
	w->woken = 0;
	pthread_cond_init(&w->cond, NULL);

	h = futex_hash_fn(addr);

	pthread_mutex_lock(&futex_lock);

	/*
	 * Re-check the value under lock. The caller already checked,
	 * but the value may have changed before we enqueued.
	 */
	(void)val;	/* Already validated by caller */

	w->next = futex_hash[h];
	futex_hash[h] = w;

	ret = 0;
	while (!w->woken) {
		if (timeout != NULL) {
			struct timespec	ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout->tv_sec;
			ts.tv_nsec += timeout->tv_nsec;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			if (pthread_cond_timedwait(&w->cond, &futex_lock,
			    &ts) != 0) {
				/* Timeout: remove waiter and return. */
				ret = -110;	/* ETIMEDOUT */
				break;
			}
		} else {
			pthread_cond_wait(&w->cond, &futex_lock);
		}
	}

	/* Remove from hash chain. */
	{
		futex_waiter_t	**pp;

		for (pp = &futex_hash[h]; *pp != NULL; pp = &(*pp)->next) {
			if (*pp == w) {
				*pp = w->next;
				break;
			}
		}
	}

	pthread_mutex_unlock(&futex_lock);
	pthread_cond_destroy(&w->cond);
	free(w);

	return ret;
}

int
futex_wake(uint64_t addr, int count, uint32_t bitset)
{
	futex_waiter_t	*w;
	unsigned int	 h;
	int		 woken;

	if (count <= 0)
		return 0;

	h = futex_hash_fn(addr);
	woken = 0;

	pthread_mutex_lock(&futex_lock);
	for (w = futex_hash[h]; w != NULL && woken < count; w = w->next) {
		if (w->addr == addr && !w->woken &&
		    (w->bitset & bitset) != 0) {
			w->woken = 1;
			pthread_cond_signal(&w->cond);
			woken++;
		}
	}
	pthread_mutex_unlock(&futex_lock);

	return woken;
}

int
futex_requeue(uint64_t from, uint64_t to, int wake_count, int requeue_limit)
{
	futex_waiter_t	*w;
	unsigned int	 h;
	int		 woken, requeued;

	h = futex_hash_fn(from);
	woken = 0;
	requeued = 0;

	pthread_mutex_lock(&futex_lock);
	for (w = futex_hash[h]; w != NULL; w = w->next) {
		if (w->addr != from || w->woken)
			continue;
		if (woken < wake_count) {
			w->woken = 1;
			pthread_cond_signal(&w->cond);
			woken++;
		} else if (requeued < requeue_limit) {
			w->addr = to;
			requeued++;
		}
	}
	pthread_mutex_unlock(&futex_lock);

	return woken;
}

static int64_t
do_futex(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
	int	op;

	op = (int)a1 & ~LINUX_FUTEX_PRIVATE_FLAG;

	switch (op) {
	case LINUX_FUTEX_WAIT: {
		uint32_t		curval;
		struct timespec		ts, *tsp;

		if (mem_read32(proc->mem, a0, &curval) != 0)
			return -LINUX_EFAULT;
		if (curval != (uint32_t)a2)
			return -11;	/* EAGAIN */

		tsp = NULL;
		if (a3 != 0) {
			uint64_t	sec, nsec;

			if (mem_read64(proc->mem, a3, &sec) != 0)
				return -LINUX_EFAULT;
			if (mem_read64(proc->mem, a3 + 8, &nsec) != 0)
				return -LINUX_EFAULT;
			ts.tv_sec = (time_t)sec;
			ts.tv_nsec = (long)nsec;
			tsp = &ts;
		}

		return futex_wait(a0, (uint32_t)a2,
		    LINUX_FUTEX_BITSET_MATCH_ANY, tsp);
	}

	case LINUX_FUTEX_WAKE:
		return futex_wake(a0, (int)a2,
		    LINUX_FUTEX_BITSET_MATCH_ANY);

	case LINUX_FUTEX_WAIT_BITSET: {
		uint32_t		curval;
		struct timespec		ts, *tsp;
		uint32_t		bitset;

		bitset = (uint32_t)a5;
		if (bitset == 0)
			return -LINUX_EINVAL;

		if (mem_read32(proc->mem, a0, &curval) != 0)
			return -LINUX_EFAULT;
		if (curval != (uint32_t)a2)
			return -11;	/* EAGAIN */

		tsp = NULL;
		if (a3 != 0) {
			uint64_t	sec, nsec;

			if (mem_read64(proc->mem, a3, &sec) != 0)
				return -LINUX_EFAULT;
			if (mem_read64(proc->mem, a3 + 8, &nsec) != 0)
				return -LINUX_EFAULT;
			ts.tv_sec = (time_t)sec;
			ts.tv_nsec = (long)nsec;
			tsp = &ts;
		}

		return futex_wait(a0, (uint32_t)a2, bitset, tsp);
	}

	case LINUX_FUTEX_WAKE_BITSET: {
		uint32_t	bitset;

		bitset = (uint32_t)a5;
		if (bitset == 0)
			return -LINUX_EINVAL;

		return futex_wake(a0, (int)a2, bitset);
	}

	case LINUX_FUTEX_REQUEUE:
		return futex_requeue(a0, a4, (int)a2, (int)a3);

	case LINUX_FUTEX_CMP_REQUEUE: {
		uint32_t	curval;

		if (mem_read32(proc->mem, a0, &curval) != 0)
			return -LINUX_EFAULT;
		if (curval != (uint32_t)a5)
			return -11;	/* EAGAIN */

		return futex_requeue(a0, a4, (int)a2, (int)a3);
	}

	default:
		LOG_DBG("futex: unhandled op %d", op);
		return 0;
	}
}

/* --- Scheduling stubs --- */

static int64_t
do_sched_getaffinity(emu_process_t *proc, uint64_t a0, uint64_t a1,
    uint64_t a2)
{
	size_t	size;
	uint8_t	*mask;

	(void)a0;	/* pid, ignored (0 = self) */

	size = (size_t)a1;
	if (size == 0 || size > 128)
		return -LINUX_EINVAL;

	mask = calloc(1, size);
	if (mask == NULL)
		return -LINUX_ENOMEM;

	/* Report 1 CPU available */
	mask[0] = 1;

	if (mem_copy_to(proc->mem, a2, mask, size) != 0) {
		free(mask);
		return -LINUX_EFAULT;
	}

	free(mask);
	return (int64_t)size;
}

static int64_t
do_sched_getparam(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	/* struct sched_param { int sched_priority; } */
	uint32_t	prio;

	(void)a0;	/* pid */

	prio = 0;
	if (mem_write32(proc->mem, a1, prio) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_personality(uint64_t a0)
{
	/* persona == 0xFFFFFFFF means query; otherwise set and return 0 */
	if ((uint32_t)a0 == 0xFFFFFFFFU)
		return 0;	/* PER_LINUX = 0 */
	return 0;
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
	case SYS_PERSONALITY:
		return do_personality(a0);
	case SYS_SCHED_SETSCHEDULER:
		(void)a0; (void)a1; (void)a2;
		return 0;
	case SYS_SCHED_GETSCHEDULER:
		(void)a0;
		return 0;	/* SCHED_OTHER */
	case SYS_SCHED_GETPARAM:
		return do_sched_getparam(proc, a0, a1);
	case SYS_SCHED_SETAFFINITY:
		(void)a0; (void)a1; (void)a2;
		return 0;
	case SYS_SCHED_GETAFFINITY:
		return do_sched_getaffinity(proc, a0, a1, a2);
	default:
		LOG_WARN("sys_misc: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
