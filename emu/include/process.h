/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include "cpu.h"
#include "memory.h"
#include "vfs.h"
#include "signal_emu.h"

/* Process states */
#define PROC_RUNNING	1
#define PROC_STOPPED	2
#define PROC_ZOMBIE	3
#define PROC_DEAD	4

/* File descriptor types */
#define FD_NONE		0
#define FD_FILE		1
#define FD_PIPE		2
#define FD_SOCKET	3
#define FD_TTY		4
#define FD_EPOLL	5
#define FD_EVENT	6
#define FD_DIR		7

#define MAX_FDS		1024
#define MAX_PROCS	256

/* File descriptor entry */
typedef struct fd_entry {
	int		type;
	int		real_fd;	/* Host file descriptor */
	int		flags;		/* O_* flags */
	int		cloexec;
	void		*private;
} fd_entry_t;

/* File descriptor table (shared between threads in a process) */
typedef struct fd_table {
	fd_entry_t	fds[MAX_FDS];
	int		refcount;
	pthread_mutex_t	lock;
} fd_table_t;

/* Emulated process */
typedef struct emu_process {
	int		pid;
	int		ppid;
	int		pgid;
	int		sid;
	int		uid, gid, euid, egid;
	int		state;
	int		exit_status;
	int		tgid;		/* Thread group ID */
	int		tid;		/* Thread ID */
	uint32_t	umask_val;

	cpu_state_t	cpu;
	mem_space_t	*mem;
	fd_table_t	*fds;
	vfs_t		*vfs;

	char		cwd[PATH_MAX];

	/* Signal state */
	struct emu_sigaction	sigactions[EMU_NSIG];
	uint64_t		sig_pending;
	uint64_t		sig_blocked;
	void			*sig_altstack;
	size_t			sig_altstack_size;

	/* Set by set_tid_address */
	uint64_t	clear_child_tid;

	/* Host thread running this process */
	pthread_t	host_thread;
	pthread_mutex_t	lock;
	pthread_cond_t	wait_cond;

	struct emu_process	*next;
} emu_process_t;

/* Process table operations */
void		 proc_table_init(void);
emu_process_t	*proc_create(emu_process_t *parent);
emu_process_t	*proc_find(int pid);
void		 proc_exit(emu_process_t *proc, int status);
int		 proc_wait(emu_process_t *parent, int pid, int *status,
		    int options);
int		 proc_fork(emu_process_t *parent);
int		 proc_execve(emu_process_t *proc, const char *path,
		    const char **argv, const char **envp);
void		 proc_destroy(emu_process_t *proc);

/* Thread entry point for running a process */
void		*proc_run(void *arg);

/* File descriptor table operations */
fd_table_t	*fd_table_create(void);
fd_table_t	*fd_table_clone(fd_table_t *);
void		 fd_table_release(fd_table_t *);
int		 fd_alloc(fd_table_t *, int minfd);
void		 fd_close(fd_table_t *, int fd);
void		 fd_close_cloexec(fd_table_t *);
fd_entry_t	*fd_get(fd_table_t *, int fd);

/* Global PID counter */
int		 proc_next_pid(void);

#endif /* PROCESS_H */
