/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef SIGNAL_EMU_H
#define SIGNAL_EMU_H

#include <stdint.h>

#define EMU_NSIG	64

/* Signal numbers (match Linux) */
#define EMU_SIGHUP	1
#define EMU_SIGINT	2
#define EMU_SIGQUIT	3
#define EMU_SIGILL	4
#define EMU_SIGTRAP	5
#define EMU_SIGABRT	6
#define EMU_SIGBUS	7
#define EMU_SIGFPE	8
#define EMU_SIGKILL	9
#define EMU_SIGUSR1	10
#define EMU_SIGSEGV	11
#define EMU_SIGUSR2	12
#define EMU_SIGPIPE	13
#define EMU_SIGALRM	14
#define EMU_SIGTERM	15
#define EMU_SIGCHLD	17
#define EMU_SIGCONT	18
#define EMU_SIGSTOP	19
#define EMU_SIGTSTP	20
#define EMU_SIGTTIN	21
#define EMU_SIGTTOU	22
#define EMU_SIGURG	23
#define EMU_SIGXCPU	24
#define EMU_SIGXFSZ	25
#define EMU_SIGVTALRM	26
#define EMU_SIGPROF	27
#define EMU_SIGWINCH	28
#define EMU_SIGIO	29
#define EMU_SIGPWR	30
#define EMU_SIGSYS	31

/* Signal action flags (match Linux) */
#define EMU_SA_NOCLDSTOP	0x00000001
#define EMU_SA_NOCLDWAIT	0x00000002
#define EMU_SA_SIGINFO		0x00000004
#define EMU_SA_RESTORER		0x04000000
#define EMU_SA_ONSTACK		0x08000000
#define EMU_SA_RESTART		0x10000000
#define EMU_SA_NODEFER		0x40000000
#define EMU_SA_RESETHAND	0x80000000

#define EMU_SIG_DFL	((uint64_t)0)
#define EMU_SIG_IGN	((uint64_t)1)

/*
 * Emulated sigaction (matches Linux kernel_sigaction for aarch64).
 * Field names avoid sa_handler/sa_flags which are macros on macOS.
 */
struct emu_sigaction {
	uint64_t	handler;	/* Handler address or SIG_DFL/SIG_IGN */
	uint64_t	flags;
	uint64_t	restorer;
	uint64_t	mask;		/* Blocked signals during handler */
};

/* Forward declaration */
typedef struct emu_process emu_process_t;

/* Signal operations */
int	sig_action(emu_process_t *proc, int sig,
	    const struct emu_sigaction *act, struct emu_sigaction *oldact);
int	sig_procmask(emu_process_t *proc, int how, const uint64_t *set,
	    uint64_t *oldset);
int	sig_send(emu_process_t *proc, int sig);
int	sig_pending(emu_process_t *proc, uint64_t *set);
void	sig_deliver(emu_process_t *proc);

#endif /* SIGNAL_EMU_H */
