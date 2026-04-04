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

#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "elf_loader.h"
#include "emu.h"
#include "jit.h"
#include "log.h"
#include "memory.h"
#include "process.h"
#include "signal_emu.h"
#include "syscall.h"
#include "vfs.h"

/*
 * JIT mode address constants.
 * Must be above iOS PAGEZERO (4GB) and away from the app binary
 * (loaded near 0x100000000 with ASLR). Using 0x500000000+ (20GB+)
 * avoids conflicts on all current iOS devices.
 */
#define JIT_BINARY_BASE		0x500000000ULL
#define JIT_INTERP_BASE		0x580000000ULL
#define JIT_STACK_TOP		0x5FFFFF0000ULL

/* Interpreter base addresses */
#define INTERP_INTERP_BASE	0x7f00000000ULL
#define INTERP_STACK_TOP	0x7fffffe000ULL

/* WNOHANG from Linux. */
#define LINUX_WNOHANG	1

/* Process table. */
static emu_process_t	*proc_list;
static pthread_mutex_t	 proc_lock = PTHREAD_MUTEX_INITIALIZER;
static int		 pid_counter;

void
proc_table_init(void)
{
	pthread_mutex_lock(&proc_lock);
	proc_list = NULL;
	pid_counter = 0;
	pthread_mutex_unlock(&proc_lock);
}

int
proc_next_pid(void)
{
	int	pid;

	pthread_mutex_lock(&proc_lock);
	pid = ++pid_counter;
	pthread_mutex_unlock(&proc_lock);

	return (pid);
}

emu_process_t *
proc_create(emu_process_t *parent)
{
	emu_process_t	*p;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return (NULL);

	p->pid = proc_next_pid();
	p->tid = p->pid;
	p->tgid = p->pid;
	p->umask_val = 022;
	p->state = PROC_RUNNING;

	if (parent != NULL) {
		p->ppid = parent->pid;
		p->pgid = parent->pgid;
		p->sid = parent->sid;
		p->uid = parent->uid;
		p->gid = parent->gid;
		p->euid = parent->euid;
		p->egid = parent->egid;
		p->umask_val = parent->umask_val;
		snprintf(p->cwd, sizeof(p->cwd), "%s", parent->cwd);
		memcpy(p->sigactions, parent->sigactions,
		    sizeof(p->sigactions));
		p->vfs = parent->vfs;
		p->fds = fd_table_clone(parent->fds);
	} else {
		snprintf(p->cwd, sizeof(p->cwd), "/");
		p->fds = fd_table_create();
	}

	if (p->fds == NULL) {
		free(p);
		return (NULL);
	}

	cpu_init(&p->cpu);
	p->mem = mem_space_create();
	if (p->mem == NULL) {
		fd_table_release(p->fds);
		free(p);
		return (NULL);
	}
	p->cpu.mem = p->mem;

	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->wait_cond, NULL);

	/* Add to process list. */
	pthread_mutex_lock(&proc_lock);
	p->next = proc_list;
	proc_list = p;
	pthread_mutex_unlock(&proc_lock);

	LOG_DBG("proc: created pid %d (parent %d)", p->pid,
	    parent ? parent->pid : 0);
	return (p);
}

void
proc_add(emu_process_t *proc)
{
	pthread_mutex_lock(&proc_lock);
	proc->next = proc_list;
	proc_list = proc;
	pthread_mutex_unlock(&proc_lock);
}

emu_process_t *
proc_find(int pid)
{
	emu_process_t	*p;

	pthread_mutex_lock(&proc_lock);
	for (p = proc_list; p != NULL; p = p->next) {
		if (p->pid == pid) {
			pthread_mutex_unlock(&proc_lock);
			return (p);
		}
	}
	pthread_mutex_unlock(&proc_lock);
	return (NULL);
}

void
proc_exit(emu_process_t *proc, int status)
{
	emu_process_t	*child, *parent;
	int		 is_thread;

	is_thread = (proc->tgid != proc->pid);

	pthread_mutex_lock(&proc->lock);
	proc->exit_status = status;
	proc->state = PROC_ZOMBIE;
	proc->cpu.running = 0;

	/* Write 0 to clear_child_tid and wake futex waiters. */
	if (proc->clear_child_tid != 0) {
		mem_write32(proc->mem, proc->clear_child_tid, 0);
		futex_wake(proc->clear_child_tid, 1,
		    LINUX_FUTEX_BITSET_MATCH_ANY);
	}
	pthread_mutex_unlock(&proc->lock);

	/*
	 * Threads don't reparent children or signal the parent process.
	 * Only the main thread (tgid == pid) does that.
	 */
	if (!is_thread) {
		/* Reparent children to pid 1. */
		pthread_mutex_lock(&proc_lock);
		for (child = proc_list; child != NULL; child = child->next) {
			if (child->ppid == proc->pid)
				child->ppid = 1;
		}
		pthread_mutex_unlock(&proc_lock);

		/* Signal parent. */
		parent = proc_find(proc->ppid);
		if (parent != NULL) {
			sig_send(parent, EMU_SIGCHLD);
			pthread_mutex_lock(&parent->lock);
			pthread_cond_broadcast(&parent->wait_cond);
			pthread_mutex_unlock(&parent->lock);
		}
	}

	LOG_DBG("proc: pid %d (tgid %d) exited with status %d",
	    proc->pid, proc->tgid, status);
}

int
proc_wait(emu_process_t *parent, int pid, int *status, int options)
{
	emu_process_t	*child;
	int		 found, ret;

	for (;;) {
		found = 0;
		ret = 0;

		pthread_mutex_lock(&proc_lock);
		for (child = proc_list; child != NULL; child = child->next) {
			if (child->ppid != parent->pid)
				continue;
			if (pid > 0 && child->pid != pid)
				continue;

			found = 1;

			if (child->state == PROC_ZOMBIE) {
				ret = child->pid;
				if (status != NULL)
					*status = child->exit_status;
				child->state = PROC_DEAD;
				pthread_mutex_unlock(&proc_lock);
				proc_destroy(child);
				return (ret);
			}
		}
		pthread_mutex_unlock(&proc_lock);

		if (!found)
			return (-ECHILD);

		if (options & LINUX_WNOHANG)
			return (0);

		/* Wait for a child state change. */
		pthread_mutex_lock(&parent->lock);
		pthread_cond_wait(&parent->wait_cond, &parent->lock);
		pthread_mutex_unlock(&parent->lock);
	}
}

int
proc_fork(emu_process_t *parent)
{
	emu_process_t	*child;
	int		 ret;

	child = proc_create(parent);
	if (child == NULL)
		return (-ENOMEM);

	/* Clone parent memory and fd table. */
	mem_space_destroy(child->mem);
	child->mem = mem_space_clone(parent->mem);
	if (child->mem == NULL) {
		proc_destroy(child);
		return (-ENOMEM);
	}

	/* Copy CPU state. */
	memcpy(&child->cpu, &parent->cpu, sizeof(cpu_state_t));
	child->cpu.mem = child->mem;

	/* Child returns 0 from fork. */
	child->cpu.x[0] = 0;

	/* Start child thread. */
	child->cpu.running = 1;
	ret = pthread_create(&child->host_thread, NULL, proc_run, child);
	if (ret != 0) {
		LOG_ERR("proc: failed to create thread for pid %d",
		    child->pid);
		proc_destroy(child);
		return (-ENOMEM);
	}
	pthread_detach(child->host_thread);

	LOG_DBG("proc: forked pid %d from pid %d", child->pid, parent->pid);
	return (child->pid);
}

int
proc_execve(emu_process_t *proc, const char *path, const char **argv,
    const char **envp)
{
	char		host_path[PATH_MAX];
	elf_info_t	info, interp_info;
	mem_space_t	*newmem;
	uint64_t	sp, entry, interp_base, stack_top, bin_base;
	int		ret, use_jit;

	/* Resolve path through VFS. */
	ret = vfs_resolve(proc->vfs, path, host_path, sizeof(host_path));
	if (ret != 0) {
		LOG_ERR("proc: execve: cannot resolve %s", path);
		return (-ENOENT);
	}

	/* Create new memory space. */
	newmem = mem_space_create();
	if (newmem == NULL)
		return (-ENOMEM);

	/* Determine if JIT mode should be used. */
	use_jit = (proc->mem != NULL && proc->mem->jit_mode) ||
	    (jit_available() && emu_jit_enabled());
	if (use_jit) {
		newmem->jit_mode = 1;
		newmem->mmap_next = MMAP_START_JIT;
		bin_base = JIT_BINARY_BASE;
		interp_base = JIT_INTERP_BASE;
		stack_top = JIT_STACK_TOP;
	} else {
		bin_base = 0;
		interp_base = INTERP_INTERP_BASE;
		stack_top = INTERP_STACK_TOP;
	}

	/* Load ELF. */
	memset(&info, 0, sizeof(info));
	ret = elf_load(host_path, newmem, bin_base, &info);
	if (ret != 0 && use_jit) {
		/* JIT mmap may have failed; fall back to interpreter. */
		LOG_WARN("proc: execve: JIT load failed, trying interpreter");
		mem_space_destroy(newmem);
		newmem = mem_space_create();
		if (newmem == NULL)
			return (-ENOMEM);
		use_jit = 0;
		bin_base = 0;
		interp_base = INTERP_INTERP_BASE;
		stack_top = INTERP_STACK_TOP;
		memset(&info, 0, sizeof(info));
		ret = elf_load(host_path, newmem, bin_base, &info);
	}
	if (ret != 0) {
		LOG_ERR("proc: execve: failed to load %s", host_path);
		mem_space_destroy(newmem);
		return (-ENOEXEC);
	}

	entry = info.entry;

	/* Load interpreter if needed. */
	if (info.interp[0] != '\0') {
		char	interp_host[PATH_MAX];

		ret = vfs_resolve(proc->vfs, info.interp,
		    interp_host, sizeof(interp_host));
		if (ret != 0) {
			LOG_ERR("proc: execve: cannot resolve interp %s",
			    info.interp);
			mem_space_destroy(newmem);
			return (-ENOENT);
		}

		memset(&interp_info, 0, sizeof(interp_info));
		ret = elf_load(interp_host, newmem, interp_base,
		    &interp_info);
		if (ret != 0) {
			LOG_ERR("proc: execve: failed to load interp %s",
			    interp_host);
			mem_space_destroy(newmem);
			return (-ENOEXEC);
		}

		info.interp_base = interp_info.base;
		info.interp_entry = interp_info.entry;
		entry = interp_info.entry;
	}

	/* Set up stack. */
	sp = elf_setup_stack(newmem, &info, argv, envp, stack_top);
	if (sp == 0) {
		LOG_ERR("proc: execve: failed to set up stack");
		mem_space_destroy(newmem);
		return (-ENOMEM);
	}

	/* Replace old memory space. */
	mem_space_destroy(proc->mem);
	proc->mem = newmem;

	/* Reset CPU. */
	cpu_init(&proc->cpu);
	proc->cpu.mem = proc->mem;
	proc->cpu.pc = entry;
	proc->cpu.sp = sp;
	proc->cpu.running = 1;

	/* Close cloexec file descriptors. */
	fd_close_cloexec(proc->fds);

	/* Reset signal handlers to defaults. */
	for (int i = 0; i < EMU_NSIG; i++) {
		if (proc->sigactions[i].handler != EMU_SIG_IGN)
			proc->sigactions[i].handler = EMU_SIG_DFL;
		proc->sigactions[i].flags = 0;
		proc->sigactions[i].mask = 0;
	}

	LOG_INFO("proc: execve pid %d: %s entry=0x%lx sp=0x%lx",
	    proc->pid, path, (unsigned long)entry, (unsigned long)sp);
	return (0);
}

void
proc_destroy(emu_process_t *proc)
{
	emu_process_t	**pp;

	if (proc == NULL)
		return;

	/* Remove from process list. */
	pthread_mutex_lock(&proc_lock);
	for (pp = &proc_list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == proc) {
			*pp = proc->next;
			break;
		}
	}
	pthread_mutex_unlock(&proc_lock);

	if (proc->mem != NULL)
		mem_space_destroy(proc->mem);
	if (proc->fds != NULL)
		fd_table_release(proc->fds);

	pthread_mutex_destroy(&proc->lock);
	pthread_cond_destroy(&proc->wait_cond);

	LOG_DBG("proc: destroyed pid %d", proc->pid);
	free(proc);
}

void *
proc_run(void *arg)
{
	emu_process_t	*proc;
	int		 ret;

	proc = (emu_process_t *)arg;

	LOG_DBG("proc: running pid %d, pc=0x%lx sp=0x%lx", proc->pid,
	    (unsigned long)proc->cpu.pc, (unsigned long)proc->cpu.sp);

#ifdef __aarch64__
	/* Use JIT native execution when available. */
	if (proc->mem != NULL && proc->mem->jit_mode && jit_available()) {
		LOG_INFO("proc: pid %d using JIT execution", proc->pid);
		jit_run(proc);
		proc_exit(proc, proc->cpu.exit_code);
		return (NULL);
	}
#endif

	while (proc->cpu.running) {
		ret = cpu_step(&proc->cpu);

		switch (ret) {
		case EMU_OK:
			break;
		case EMU_SYSCALL:
			sys_handle(proc);
			break;
		case EMU_SEGFAULT:
			LOG_ERR("proc: pid %d SIGSEGV at pc=0x%lx",
			    proc->pid, (unsigned long)proc->cpu.pc);
			sig_send(proc, EMU_SIGSEGV);
			break;
		case EMU_UNIMPL:
			LOG_ERR("proc: pid %d unimplemented insn at pc=0x%lx",
			    proc->pid, (unsigned long)proc->cpu.pc);
			proc_exit(proc, 128 + EMU_SIGILL);
			return (NULL);
		case EMU_BREAK:
			LOG_DBG("proc: pid %d breakpoint at pc=0x%lx",
			    proc->pid, (unsigned long)proc->cpu.pc);
			break;
		case EMU_EXIT:
			proc_exit(proc, proc->cpu.exit_code);
			return (NULL);
		default:
			LOG_ERR("proc: pid %d unexpected cpu_step ret=%d",
			    proc->pid, ret);
			proc_exit(proc, 1);
			return (NULL);
		}

		sig_deliver(proc);
	}

	return (NULL);
}

/* File descriptor table operations. */

fd_table_t *
fd_table_create(void)
{
	fd_table_t	*t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return (NULL);

	t->refcount = 1;
	pthread_mutex_init(&t->lock, NULL);
	return (t);
}

fd_table_t *
fd_table_clone(fd_table_t *src)
{
	fd_table_t	*t;
	int		 i, newfd;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return (NULL);

	t->refcount = 1;
	pthread_mutex_init(&t->lock, NULL);

	pthread_mutex_lock(&src->lock);
	for (i = 0; i < MAX_FDS; i++) {
		if (src->fds[i].type == FD_NONE)
			continue;

		t->fds[i] = src->fds[i];

		/* Duplicate the host fd. */
		if (src->fds[i].real_fd >= 0) {
			newfd = dup(src->fds[i].real_fd);
			if (newfd < 0) {
				t->fds[i].type = FD_NONE;
				continue;
			}
			t->fds[i].real_fd = newfd;
		}
	}
	pthread_mutex_unlock(&src->lock);

	return (t);
}

void
fd_table_release(fd_table_t *tbl)
{
	int	i, rc;

	if (tbl == NULL)
		return;

	pthread_mutex_lock(&tbl->lock);
	rc = --tbl->refcount;
	pthread_mutex_unlock(&tbl->lock);

	if (rc > 0)
		return;

	for (i = 0; i < MAX_FDS; i++) {
		if (tbl->fds[i].type != FD_NONE) {
			if (tbl->fds[i].real_fd >= 0)
				close(tbl->fds[i].real_fd);
			if (tbl->fds[i].private != NULL)
				free(tbl->fds[i].private);
		}
	}

	pthread_mutex_destroy(&tbl->lock);
	free(tbl);
}

int
fd_alloc(fd_table_t *tbl, int minfd)
{
	int	i;

	if (minfd < 0)
		minfd = 0;

	pthread_mutex_lock(&tbl->lock);
	for (i = minfd; i < MAX_FDS; i++) {
		if (tbl->fds[i].type == FD_NONE) {
			pthread_mutex_unlock(&tbl->lock);
			return (i);
		}
	}
	pthread_mutex_unlock(&tbl->lock);
	return (-EMFILE);
}

void
fd_close(fd_table_t *tbl, int fd)
{
	if (fd < 0 || fd >= MAX_FDS)
		return;

	pthread_mutex_lock(&tbl->lock);
	if (tbl->fds[fd].type != FD_NONE) {
		if (tbl->fds[fd].real_fd >= 0)
			close(tbl->fds[fd].real_fd);
		if (tbl->fds[fd].private != NULL)
			free(tbl->fds[fd].private);
		memset(&tbl->fds[fd], 0, sizeof(fd_entry_t));
	}
	pthread_mutex_unlock(&tbl->lock);
}

void
fd_close_cloexec(fd_table_t *tbl)
{
	int	i;

	pthread_mutex_lock(&tbl->lock);
	for (i = 0; i < MAX_FDS; i++) {
		if (tbl->fds[i].type != FD_NONE && tbl->fds[i].cloexec) {
			if (tbl->fds[i].real_fd >= 0)
				close(tbl->fds[i].real_fd);
			if (tbl->fds[i].private != NULL)
				free(tbl->fds[i].private);
			memset(&tbl->fds[i], 0, sizeof(fd_entry_t));
		}
	}
	pthread_mutex_unlock(&tbl->lock);
}

fd_entry_t *
fd_get(fd_table_t *tbl, int fd)
{
	if (fd < 0 || fd >= MAX_FDS)
		return (NULL);
	if (tbl->fds[fd].type == FD_NONE)
		return (NULL);
	return (&tbl->fds[fd]);
}
