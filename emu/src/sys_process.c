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

/* Use shared clone flag definitions from syscall.h */

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
	emu_process_t	*child;
	int		 ret, is_thread;

	is_thread = (flags & LINUX_CLONE_THREAD) != 0;

	/*
	 * CLONE_THREAD implies CLONE_VM, CLONE_FILES, CLONE_SIGHAND.
	 */
	if (is_thread)
		flags |= LINUX_CLONE_VM | LINUX_CLONE_FILES |
		    LINUX_CLONE_SIGHAND;

	if (flags & LINUX_CLONE_VM) {
		/*
		 * Thread-like clone: allocate a new process struct
		 * but share memory and optionally fd table.
		 */
		child = calloc(1, sizeof(*child));
		if (child == NULL)
			return -LINUX_ENOMEM;

		child->pid = proc_next_pid();
		child->tid = child->pid;
		child->ppid = proc->ppid;
		child->pgid = proc->pgid;
		child->sid = proc->sid;
		child->uid = proc->uid;
		child->gid = proc->gid;
		child->euid = proc->euid;
		child->egid = proc->egid;
		child->umask_val = proc->umask_val;
		child->state = PROC_RUNNING;
		child->vfs = proc->vfs;
		snprintf(child->cwd, sizeof(child->cwd), "%s", proc->cwd);
		memcpy(child->sigactions, proc->sigactions,
		    sizeof(child->sigactions));

		/* Share memory space (increment refcount). */
		child->mem = proc->mem;
		mem_space_ref(child->mem);

		/* Share or clone fd table. */
		if (flags & LINUX_CLONE_FILES) {
			child->fds = proc->fds;
			pthread_mutex_lock(&child->fds->lock);
			child->fds->refcount++;
			pthread_mutex_unlock(&child->fds->lock);
		} else {
			child->fds = fd_table_clone(proc->fds);
			if (child->fds == NULL) {
				mem_space_destroy(child->mem);
				free(child);
				return -LINUX_ENOMEM;
			}
		}

		/* Thread group: child shares parent's tgid. */
		if (is_thread)
			child->tgid = proc->tgid;
		else
			child->tgid = child->pid;

		/* Copy CPU state and set child return value. */
		memcpy(&child->cpu, &proc->cpu, sizeof(cpu_state_t));
		child->cpu.mem = child->mem;
		child->cpu.x[0] = 0;
		child->cpu.running = 1;

		/* Set child stack. */
		if (newsp != 0)
			child->cpu.sp = newsp;

		/* Set TLS. */
		if (flags & LINUX_CLONE_SETTLS)
			child->cpu.tpidr_el0 = tls;

		pthread_mutex_init(&child->lock, NULL);
		pthread_cond_init(&child->wait_cond, NULL);

		/* Handle CLONE_CHILD_SETTID. */
		if ((flags & LINUX_CLONE_CHILD_SETTID) &&
		    child_tidptr != 0) {
			mem_write32(child->mem, child_tidptr,
			    (uint32_t)child->tid);
		}

		/* Handle CLONE_CHILD_CLEARTID. */
		if ((flags & LINUX_CLONE_CHILD_CLEARTID) &&
		    child_tidptr != 0) {
			child->clear_child_tid = child_tidptr;
		}

		/* Handle CLONE_PARENT_SETTID. */
		if ((flags & LINUX_CLONE_PARENT_SETTID) &&
		    parent_tidptr != 0) {
			mem_write32(proc->mem, parent_tidptr,
			    (uint32_t)child->tid);
		}

		/* Add to process list. */
		proc_add(child);

		/* Start child on a new pthread. */
		ret = pthread_create(&child->host_thread, NULL,
		    proc_run, child);
		if (ret != 0) {
			LOG_ERR("clone: failed to create thread for pid %d",
			    child->pid);
			proc_destroy(child);
			return -LINUX_ENOMEM;
		}
		pthread_detach(child->host_thread);

		LOG_DBG("clone: thread parent=%d child=%d flags=0x%llx",
		    proc->pid, child->pid, (unsigned long long)flags);

		return child->tid;
	}

	/* Non-CLONE_VM: traditional fork with flag handling. */
	ret = proc_fork(proc);
	if (ret < 0)
		return -LINUX_ENOMEM;

	child = proc_find(ret);
	if (child == NULL)
		return -LINUX_ENOMEM;

	/* Set child stack if provided. */
	if (newsp != 0)
		child->cpu.sp = newsp;

	/* Handle CLONE_SETTLS. */
	if (flags & LINUX_CLONE_SETTLS)
		child->cpu.tpidr_el0 = tls;

	/* Handle CLONE_CHILD_SETTID. */
	if ((flags & LINUX_CLONE_CHILD_SETTID) && child_tidptr != 0)
		mem_write32(child->mem, child_tidptr, (uint32_t)child->tid);

	/* Handle CLONE_CHILD_CLEARTID. */
	if ((flags & LINUX_CLONE_CHILD_CLEARTID) && child_tidptr != 0)
		child->clear_child_tid = child_tidptr;

	/* Handle CLONE_PARENT_SETTID. */
	if ((flags & LINUX_CLONE_PARENT_SETTID) && parent_tidptr != 0)
		mem_write32(proc->mem, parent_tidptr, (uint32_t)child->tid);

	LOG_DBG("clone: fork parent=%d child=%d flags=0x%llx",
	    proc->pid, child->pid, (unsigned long long)flags);

	return child->tid;
}

static int64_t
do_clone3(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	/*
	 * struct clone_args layout:
	 *   0: flags(8), 8: pidfd(8), 16: child_tid(8),
	 *  24: parent_tid(8), 32: exit_signal(8), 40: stack(8),
	 *  48: stack_size(8), 56: tls(8), 64: set_tid(8),
	 *  72: set_tid_size(8), 80: cgroup(8)
	 */
	uint64_t	flags, newsp, stack_size, tls;
	uint64_t	parent_tidptr, child_tidptr, exit_signal;

	(void)a1;	/* size of struct, validated elsewhere */

	if (mem_read64(proc->mem, a0, &flags) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 16, &child_tidptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 24, &parent_tidptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 32, &exit_signal) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 40, &newsp) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 48, &stack_size) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a0 + 56, &tls) != 0)
		return -LINUX_EFAULT;

	(void)exit_signal;	/* Accepted but not acted on */

	/* Stack grows downward: point SP to top of the stack region. */
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
