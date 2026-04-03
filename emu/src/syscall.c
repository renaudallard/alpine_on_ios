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

#include <stdint.h>
#include <sys/types.h>

#include "syscall.h"
#include "process.h"
#include "log.h"

/* Linux ENOSYS */
#define LINUX_ENOSYS	38

void
sys_handle(emu_process_t *proc)
{
	uint64_t	nr, a0, a1, a2, a3, a4, a5;
	int64_t		ret;

	nr = proc->cpu.x[8];
	a0 = proc->cpu.x[0];
	a1 = proc->cpu.x[1];
	a2 = proc->cpu.x[2];
	a3 = proc->cpu.x[3];
	a4 = proc->cpu.x[4];
	a5 = proc->cpu.x[5];

	LOG_TRACE("syscall: nr=%llu a0=0x%llx a1=0x%llx a2=0x%llx",
	    (unsigned long long)nr, (unsigned long long)a0,
	    (unsigned long long)a1, (unsigned long long)a2);

	switch (nr) {
	/* File I/O */
	case SYS_OPENAT:
	case SYS_CLOSE:
	case SYS_READ:
	case SYS_WRITE:
	case SYS_READV:
	case SYS_WRITEV:
	case SYS_PREAD64:
	case SYS_PWRITE64:
	case SYS_LSEEK:
	case SYS_IOCTL:
	case SYS_FCNTL:
	case SYS_DUP:
	case SYS_DUP3:
	case SYS_PIPE2:
	case SYS_GETDENTS64:
	case SYS_NEWFSTATAT:
	case SYS_FSTAT:
	case SYS_READLINKAT:
	case SYS_FACCESSAT:
	case SYS_MKDIRAT:
	case SYS_UNLINKAT:
	case SYS_SYMLINKAT:
	case SYS_RENAMEAT:
	case SYS_FCHMODAT:
	case SYS_FCHMOD:
	case SYS_FCHOWNAT:
	case SYS_FCHOWN:
	case SYS_FTRUNCATE:
	case SYS_TRUNCATE:
	case SYS_FSYNC:
	case SYS_FDATASYNC:
	case SYS_UTIMENSAT:
	case SYS_FLOCK:
	case SYS_STATFS:
	case SYS_FSTATFS:
	case SYS_STATX:
	case SYS_SENDFILE:
	case SYS_FACCESSAT2:
	case SYS_RENAMEAT2:
	case SYS_LINKAT:
	case SYS_MKNODAT:
	case SYS_SYNC:
	case SYS_EPOLL_CREATE1:
	case SYS_EPOLL_CTL:
	case SYS_EPOLL_PWAIT:
	case SYS_EVENTFD2:
	case SYS_TIMERFD_CREATE:
	case SYS_TIMERFD_SETTIME:
	case SYS_PPOLL:
	case SYS_PSELECT6:
		ret = sys_file(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	/* Process management */
	case SYS_EXIT:
	case SYS_EXIT_GROUP:
	case SYS_CLONE:
	case SYS_CLONE3:
	case SYS_EXECVE:
	case SYS_WAIT4:
	case SYS_GETPID:
	case SYS_GETPPID:
	case SYS_GETUID:
	case SYS_GETEUID:
	case SYS_GETGID:
	case SYS_GETEGID:
	case SYS_GETTID:
	case SYS_SETUID:
	case SYS_SETGID:
	case SYS_SETPGID:
	case SYS_GETPGID:
	case SYS_SETSID:
	case SYS_GETGROUPS:
	case SYS_SETGROUPS:
	case SYS_SET_TID_ADDRESS:
	case SYS_SET_ROBUST_LIST:
	case SYS_GET_ROBUST_LIST:
	case SYS_PRCTL:
	case SYS_PRLIMIT64:
	case SYS_SCHED_YIELD:
	case SYS_GETRLIMIT:
	case SYS_SETRLIMIT:
	case SYS_GETRUSAGE:
	case SYS_TIMES:
	case SYS_RSEQ:
	case SYS_SETREUID:
	case SYS_SETREGID:
	case SYS_SETRESUID:
	case SYS_GETRESUID:
	case SYS_SETRESGID:
	case SYS_GETRESGID:
	case SYS_SETPRIORITY:
	case SYS_GETPRIORITY:
		ret = sys_process(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	/* Memory management */
	case SYS_BRK:
	case SYS_MMAP:
	case SYS_MUNMAP:
	case SYS_MPROTECT:
	case SYS_MREMAP:
	case SYS_MADVISE:
	case SYS_MSYNC:
	case SYS_MEMFD_CREATE:
	case SYS_MLOCK:
	case SYS_MUNLOCK:
	case SYS_MLOCKALL:
	case SYS_MUNLOCKALL:
	case SYS_MINCORE:
		ret = sys_memory(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	/* Signals */
	case SYS_RT_SIGACTION:
	case SYS_RT_SIGPROCMASK:
	case SYS_RT_SIGPENDING:
	case SYS_RT_SIGRETURN:
	case SYS_RT_SIGSUSPEND:
	case SYS_RT_SIGTIMEDWAIT:
	case SYS_KILL:
	case SYS_TKILL:
	case SYS_TGKILL:
	case SYS_SIGALTSTACK:
		ret = sys_signal(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	/* Network */
	case SYS_SOCKET:
	case SYS_SOCKETPAIR:
	case SYS_BIND:
	case SYS_LISTEN:
	case SYS_ACCEPT:
	case SYS_ACCEPT4:
	case SYS_CONNECT:
	case SYS_GETSOCKNAME:
	case SYS_GETPEERNAME:
	case SYS_SENDTO:
	case SYS_RECVFROM:
	case SYS_SETSOCKOPT:
	case SYS_GETSOCKOPT:
	case SYS_SHUTDOWN:
	case SYS_SENDMSG:
	case SYS_RECVMSG:
		ret = sys_net(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	/* Miscellaneous */
	case SYS_UNAME:
	case SYS_GETCWD:
	case SYS_CHDIR:
	case SYS_FCHDIR:
	case SYS_CHROOT:
	case SYS_UMASK:
	case SYS_GETTIMEOFDAY:
	case SYS_CLOCK_GETTIME:
	case SYS_CLOCK_GETRES:
	case SYS_CLOCK_NANOSLEEP:
	case SYS_NANOSLEEP:
	case SYS_GETITIMER:
	case SYS_SETITIMER:
	case SYS_SYSINFO:
	case SYS_GETRANDOM:
	case SYS_FUTEX:
	case SYS_SYSLOG:
	case SYS_PERSONALITY:
	case SYS_SCHED_SETSCHEDULER:
	case SYS_SCHED_GETSCHEDULER:
	case SYS_SCHED_GETPARAM:
	case SYS_SCHED_SETAFFINITY:
	case SYS_SCHED_GETAFFINITY:
		ret = sys_misc(proc, (int)nr, a0, a1, a2, a3, a4, a5);
		break;

	default:
		LOG_WARN("unhandled syscall %llu", (unsigned long long)nr);
		ret = -LINUX_ENOSYS;
		break;
	}

	proc->cpu.x[0] = (uint64_t)ret;

	LOG_TRACE("syscall: nr=%llu ret=%lld",
	    (unsigned long long)nr, (long long)ret);
}
