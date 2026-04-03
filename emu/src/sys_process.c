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

#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "log.h"

/* Linux errno */
#define LINUX_EPERM	1
#define LINUX_ESRCH	3
#define LINUX_ECHILD	10
#define LINUX_ENOMEM	12
#define LINUX_EFAULT	14
#define LINUX_EINVAL	22
#define LINUX_ENOSYS	38

/* Linux clone flags */
#define LINUX_CLONE_VM		0x00000100
#define LINUX_CLONE_FS		0x00000200
#define LINUX_CLONE_FILES	0x00000400
#define LINUX_CLONE_SIGHAND	0x00000800
#define LINUX_CLONE_THREAD	0x00010000
#define LINUX_CLONE_CHILD_SETTID	0x01000000
#define LINUX_CLONE_CHILD_CLEARTID	0x00200000
#define LINUX_CLONE_PARENT_SETTID	0x00100000

/* Linux wait options */
#define LINUX_WNOHANG		1
#define LINUX_WUNTRACED		2

/* Linux prctl constants */
#define LINUX_PR_SET_NAME	15
#define LINUX_PR_GET_NAME	16

/* Linux RLIMIT constants */
#define LINUX_RLIMIT_CPU	0
#define LINUX_RLIMIT_FSIZE	1
#define LINUX_RLIMIT_DATA	2
#define LINUX_RLIMIT_STACK	3
#define LINUX_RLIMIT_CORE	4
#define LINUX_RLIMIT_NOFILE	7
#define LINUX_RLIMIT_AS		9

#define LINUX_RLIM_INFINITY	(~(uint64_t)0)

/* Linux struct rlimit layout: cur(8) + max(8) */

static int64_t
do_exit(emu_process_t *proc, uint64_t a0)
{
	int	status;

	status = (int)a0;
	proc->exit_status = (status & 0xff) << 8;
	proc->state = PROC_ZOMBIE;
	proc->cpu.running = 0;

	LOG_DBG("exit: pid=%d status=%d", proc->pid, status);
	return 0;
}

static int64_t
do_clone(emu_process_t *proc, uint64_t flags, uint64_t newsp,
    uint64_t parent_tidptr, uint64_t tls, uint64_t child_tidptr)
{
	int	childpid;

	if (flags & LINUX_CLONE_THREAD) {
		/* Thread creation: share memory and fds. */
		LOG_WARN("clone: CLONE_THREAD not fully supported");
		return -LINUX_ENOSYS;
	}

	childpid = proc_fork(proc);
	if (childpid < 0)
		return -LINUX_ENOMEM;

	/* Set child stack if provided. */
	if (newsp != 0) {
		emu_process_t	*child;

		child = proc_find(childpid);
		if (child != NULL)
			child->cpu.sp = newsp;
	}

	/* Handle CLONE_CHILD_SETTID. */
	if (flags & LINUX_CLONE_CHILD_SETTID && child_tidptr != 0) {
		emu_process_t	*child;

		child = proc_find(childpid);
		if (child != NULL) {
			int32_t	tid;

			tid = (int32_t)child->tid;
			mem_write32(child->mem, child_tidptr, (uint32_t)tid);
		}
	}

	/* Handle CLONE_CHILD_CLEARTID. */
	if (flags & LINUX_CLONE_CHILD_CLEARTID && child_tidptr != 0) {
		emu_process_t	*child;

		child = proc_find(childpid);
		if (child != NULL)
			child->clear_child_tid = child_tidptr;
	}

	/* Handle CLONE_PARENT_SETTID. */
	if (flags & LINUX_CLONE_PARENT_SETTID && parent_tidptr != 0) {
		int32_t	tid;

		tid = (int32_t)childpid;
		mem_write32(proc->mem, parent_tidptr, (uint32_t)tid);
	}

	/* Handle TLS. */
	if (tls != 0) {
		emu_process_t	*child;

		child = proc_find(childpid);
		if (child != NULL)
			child->cpu.tpidr_el0 = tls;
	}

	LOG_DBG("clone: parent=%d child=%d flags=0x%llx",
	    proc->pid, childpid, (unsigned long long)flags);

	return childpid;
}

static int64_t
do_clone3(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	/*
	 * clone3 struct layout (first fields):
	 * flags(8), pidfd(8), child_tid(8), parent_tid(8),
	 * exit_signal(8), stack(8), stack_size(8), tls(8)
	 */
	uint64_t	flags, newsp, stack_size, tls;
	uint64_t	parent_tidptr, child_tidptr;

	(void)a1;

	if (mem_read64(proc->mem, a0, &flags) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 16, &child_tidptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 24, &parent_tidptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 40, &newsp) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 48, &stack_size) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 56, &tls) != 0)
		return -LINUX_EFAULT;

	if (newsp != 0 && stack_size != 0)
		newsp += stack_size;

	return do_clone(proc, flags, newsp, parent_tidptr, tls,
	    child_tidptr);
}

static int64_t
do_execve(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	char		path[PATH_MAX];
	const char	**argv, **envp;
	uint64_t	ptr;
	int		i, count, ret;

	if (mem_read_str(proc->mem, a0, path, sizeof(path)) != 0)
		return -LINUX_EFAULT;

	/* Read argv. */
	count = 0;
	if (a1 != 0) {
		for (i = 0; i < 256; i++) {
			if (mem_read64(proc->mem, a1 + (uint64_t)i * 8,
			    &ptr) != 0)
				return -LINUX_EFAULT;
			if (ptr == 0)
				break;
			count++;
		}
	}

	argv = calloc((size_t)count + 1, sizeof(char *));
	if (argv == NULL)
		return -LINUX_ENOMEM;

	for (i = 0; i < count; i++) {
		char	*arg;

		if (mem_read64(proc->mem, a1 + (uint64_t)i * 8, &ptr) != 0) {
			free(argv);
			return -LINUX_EFAULT;
		}
		arg = malloc(PATH_MAX);
		if (arg == NULL) {
			while (--i >= 0)
				free((void *)argv[i]);
			free(argv);
			return -LINUX_ENOMEM;
		}
		if (mem_read_str(proc->mem, ptr, arg, PATH_MAX) != 0) {
			free(arg);
			while (--i >= 0)
				free((void *)argv[i]);
			free(argv);
			return -LINUX_EFAULT;
		}
		argv[i] = arg;
	}
	argv[count] = NULL;

	/* Read envp. */
	count = 0;
	if (a2 != 0) {
		for (i = 0; i < 1024; i++) {
			if (mem_read64(proc->mem, a2 + (uint64_t)i * 8,
			    &ptr) != 0)
				break;
			if (ptr == 0)
				break;
			count++;
		}
	}

	envp = calloc((size_t)count + 1, sizeof(char *));
	if (envp == NULL) {
		for (i = 0; argv[i] != NULL; i++)
			free((void *)argv[i]);
		free(argv);
		return -LINUX_ENOMEM;
	}

	for (i = 0; i < count; i++) {
		char	*env;

		if (mem_read64(proc->mem, a2 + (uint64_t)i * 8, &ptr) != 0) {
			free(envp);
			for (i = 0; argv[i] != NULL; i++)
				free((void *)argv[i]);
			free(argv);
			return -LINUX_EFAULT;
		}
		env = malloc(4096);
		if (env == NULL) {
			while (--i >= 0)
				free((void *)envp[i]);
			free(envp);
			for (i = 0; argv[i] != NULL; i++)
				free((void *)argv[i]);
			free(argv);
			return -LINUX_ENOMEM;
		}
		if (mem_read_str(proc->mem, ptr, env, 4096) != 0) {
			free(env);
			while (--i >= 0)
				free((void *)envp[i]);
			free(envp);
			for (i = 0; argv[i] != NULL; i++)
				free((void *)argv[i]);
			free(argv);
			return -LINUX_EFAULT;
		}
		envp[i] = env;
	}
	envp[count] = NULL;

	ret = proc_execve(proc, path, argv, envp);

	for (i = 0; argv[i] != NULL; i++)
		free((void *)argv[i]);
	free(argv);
	for (i = 0; envp[i] != NULL; i++)
		free((void *)envp[i]);
	free(envp);

	return ret;
}

static int64_t
do_wait4(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int	pid, options, status, ret;

	(void)a3;	/* rusage, ignored */

	pid = (int)(int32_t)a0;
	options = (int)a2;

	ret = proc_wait(proc, pid, &status, options);
	if (ret < 0)
		return -LINUX_ECHILD;

	if (a1 != 0) {
		if (mem_write32(proc->mem, a1, (uint32_t)status) != 0)
			return -LINUX_EFAULT;
	}

	return ret;
}

static int64_t
do_prlimit64(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	uint64_t	cur, max;

	(void)a0;	/* pid, ignored (always self) */

	switch ((int)a1) {
	case LINUX_RLIMIT_STACK:
		cur = 8 * 1024 * 1024;
		max = LINUX_RLIM_INFINITY;
		break;
	case LINUX_RLIMIT_NOFILE:
		cur = MAX_FDS;
		max = MAX_FDS;
		break;
	case LINUX_RLIMIT_AS:
	case LINUX_RLIMIT_DATA:
		cur = LINUX_RLIM_INFINITY;
		max = LINUX_RLIM_INFINITY;
		break;
	case LINUX_RLIMIT_CORE:
		cur = 0;
		max = 0;
		break;
	case LINUX_RLIMIT_FSIZE:
	case LINUX_RLIMIT_CPU:
	default:
		cur = LINUX_RLIM_INFINITY;
		max = LINUX_RLIM_INFINITY;
		break;
	}

	/* Read new limit if provided. */
	if (a2 != 0) {
		/* Accept but ignore new limits. */
	}

	/* Write old limit if requested. */
	if (a3 != 0) {
		if (mem_write64(proc->mem, a3, cur) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a3 + 8, max) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_prctl(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	(void)a2;
	(void)a3;
	(void)a4;

	switch ((int)a0) {
	case LINUX_PR_SET_NAME:
		/* Thread name: read and ignore. */
		return 0;
	case LINUX_PR_GET_NAME: {
		char	name[16];

		memset(name, 0, sizeof(name));
		snprintf(name, sizeof(name), "emu-%d", proc->pid);
		if (mem_copy_to(proc->mem, a1, name, 16) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	default:
		LOG_DBG("prctl: unhandled option %lld", (long long)a0);
		return 0;
	}
}

int64_t
sys_process(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	(void)a5;

	switch (nr) {
	case SYS_EXIT:
	case SYS_EXIT_GROUP:
		return do_exit(proc, a0);

	case SYS_GETPID:
		return proc->pid;
	case SYS_GETPPID:
		return proc->ppid;
	case SYS_GETUID:
		return proc->uid;
	case SYS_GETEUID:
		return proc->euid;
	case SYS_GETGID:
		return proc->gid;
	case SYS_GETEGID:
		return proc->egid;
	case SYS_GETTID:
		return proc->tid;

	case SYS_SET_TID_ADDRESS:
		proc->clear_child_tid = a0;
		return proc->tid;

	case SYS_SET_ROBUST_LIST:
		return 0;
	case SYS_GET_ROBUST_LIST:
		return 0;

	case SYS_CLONE:
		return do_clone(proc, a0, a1, a2, a3, a4);
	case SYS_CLONE3:
		return do_clone3(proc, a0, a1);
	case SYS_EXECVE:
		return do_execve(proc, a0, a1, a2);
	case SYS_WAIT4:
		return do_wait4(proc, a0, a1, a2, a3);

	case SYS_SETPGID:
		if (a0 == 0 || (int)(int32_t)a0 == proc->pid)
			proc->pgid = (a1 == 0) ? proc->pid : (int)(int32_t)a1;
		return 0;
	case SYS_GETPGID:
		if (a0 == 0 || (int)(int32_t)a0 == proc->pid)
			return proc->pgid;
		return -LINUX_ESRCH;
	case SYS_SETSID:
		proc->sid = proc->pid;
		proc->pgid = proc->pid;
		return proc->pid;

	case SYS_SETUID:
		proc->uid = proc->euid = (int)(int32_t)a0;
		return 0;
	case SYS_SETGID:
		proc->gid = proc->egid = (int)(int32_t)a0;
		return 0;
	case SYS_SETREUID:
		if ((int32_t)a0 != -1)
			proc->uid = (int)(int32_t)a0;
		if ((int32_t)a1 != -1)
			proc->euid = (int)(int32_t)a1;
		return 0;
	case SYS_SETREGID:
		if ((int32_t)a0 != -1)
			proc->gid = (int)(int32_t)a0;
		if ((int32_t)a1 != -1)
			proc->egid = (int)(int32_t)a1;
		return 0;
	case SYS_SETRESUID:
		if ((int32_t)a0 != -1)
			proc->uid = (int)(int32_t)a0;
		if ((int32_t)a1 != -1)
			proc->euid = (int)(int32_t)a1;
		/* saved uid (a2) ignored */
		return 0;
	case SYS_SETRESGID:
		if ((int32_t)a0 != -1)
			proc->gid = (int)(int32_t)a0;
		if ((int32_t)a1 != -1)
			proc->egid = (int)(int32_t)a1;
		return 0;
	case SYS_GETRESUID:
		if (a0 != 0)
			mem_write32(proc->mem, a0, (uint32_t)proc->uid);
		if (a1 != 0)
			mem_write32(proc->mem, a1, (uint32_t)proc->euid);
		if (a2 != 0)
			mem_write32(proc->mem, a2, (uint32_t)proc->uid);
		return 0;
	case SYS_GETRESGID:
		if (a0 != 0)
			mem_write32(proc->mem, a0, (uint32_t)proc->gid);
		if (a1 != 0)
			mem_write32(proc->mem, a1, (uint32_t)proc->egid);
		if (a2 != 0)
			mem_write32(proc->mem, a2, (uint32_t)proc->gid);
		return 0;

	case SYS_GETGROUPS:
		/* No supplementary groups. */
		return 0;
	case SYS_SETGROUPS:
		return 0;

	case SYS_PRCTL:
		return do_prctl(proc, a0, a1, a2, a3, a4);
	case SYS_PRLIMIT64:
		return do_prlimit64(proc, a0, a1, a2, a3);
	case SYS_GETRLIMIT:
		return do_prlimit64(proc, 0, a0, 0, a1);
	case SYS_SETRLIMIT:
		return do_prlimit64(proc, 0, a0, a1, 0);
	case SYS_GETRUSAGE: {
		/* Return zeroed rusage. */
		uint8_t	buf[144];

		memset(buf, 0, sizeof(buf));
		if (a1 != 0)
			mem_copy_to(proc->mem, a1, buf, sizeof(buf));
		return 0;
	}

	case SYS_SCHED_YIELD:
		sched_yield();
		return 0;

	case SYS_TIMES:
		/* Return 0 clock ticks. */
		if (a0 != 0) {
			uint8_t	buf[32];

			memset(buf, 0, sizeof(buf));
			mem_copy_to(proc->mem, a0, buf, sizeof(buf));
		}
		return 0;

	case SYS_RSEQ:
		return -LINUX_ENOSYS;

	case SYS_SETPRIORITY:
	case SYS_GETPRIORITY:
		return 0;

	default:
		LOG_WARN("sys_process: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
