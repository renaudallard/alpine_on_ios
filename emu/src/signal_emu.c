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

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "process.h"
#include "signal_emu.h"

/* sigprocmask how values (match Linux). */
#define SIG_BLOCK	0
#define SIG_UNBLOCK	1
#define SIG_SETMASK	2

/* Bitmask for a signal number (1-based). */
#define SIGMASK(sig)	(1ULL << ((sig) - 1))

/* Signals that cannot be caught or blocked. */
#define UNCATCHABLE	(SIGMASK(EMU_SIGKILL) | SIGMASK(EMU_SIGSTOP))

int
sig_action(emu_process_t *proc, int sig,
    const struct emu_sigaction *act, struct emu_sigaction *oldact)
{
	if (sig < 1 || sig >= EMU_NSIG)
		return (-EINVAL);

	if (oldact != NULL)
		memcpy(oldact, &proc->sigactions[sig], sizeof(*oldact));

	if (act != NULL) {
		/* Cannot change handler for SIGKILL or SIGSTOP. */
		if (sig == EMU_SIGKILL || sig == EMU_SIGSTOP)
			return (-EINVAL);
		memcpy(&proc->sigactions[sig], act, sizeof(*act));
	}

	return (0);
}

int
sig_procmask(emu_process_t *proc, int how, const uint64_t *set,
    uint64_t *oldset)
{
	if (oldset != NULL)
		*oldset = proc->sig_blocked;

	if (set != NULL) {
		switch (how) {
		case SIG_BLOCK:
			proc->sig_blocked |= *set;
			break;
		case SIG_UNBLOCK:
			proc->sig_blocked &= ~(*set);
			break;
		case SIG_SETMASK:
			proc->sig_blocked = *set;
			break;
		default:
			return (-EINVAL);
		}

		/* SIGKILL and SIGSTOP can never be blocked. */
		proc->sig_blocked &= ~UNCATCHABLE;
	}

	return (0);
}

int
sig_send(emu_process_t *proc, int sig)
{
	if (sig < 1 || sig >= EMU_NSIG)
		return (-EINVAL);
	if (sig == 0)
		return (0);	/* Signal 0 is used for permission check. */

	proc->sig_pending |= SIGMASK(sig);
	return (0);
}

int
sig_pending(emu_process_t *proc, uint64_t *set)
{
	*set = proc->sig_pending & ~proc->sig_blocked;
	return (0);
}

/*
 * Check if a signal's default action is to ignore.
 */
static int
sig_default_ignore(int sig)
{
	switch (sig) {
	case EMU_SIGCHLD:
	case EMU_SIGURG:
	case EMU_SIGWINCH:
		return (1);
	default:
		return (0);
	}
}

/*
 * Check if a signal's default action is to stop.
 */
static int
sig_default_stop(int sig)
{
	switch (sig) {
	case EMU_SIGSTOP:
	case EMU_SIGTSTP:
	case EMU_SIGTTIN:
	case EMU_SIGTTOU:
		return (1);
	default:
		return (0);
	}
}

void
sig_deliver(emu_process_t *proc)
{
	uint64_t		 deliverable;
	int			 sig;
	struct emu_sigaction	*sa;

	deliverable = proc->sig_pending & ~proc->sig_blocked;
	if (deliverable == 0)
		return;

	/* Find lowest set bit (first pending unblocked signal). */
	for (sig = 1; sig < EMU_NSIG; sig++) {
		if (deliverable & SIGMASK(sig))
			break;
	}
	if (sig >= EMU_NSIG)
		return;

	/* Clear from pending. */
	proc->sig_pending &= ~SIGMASK(sig);

	sa = &proc->sigactions[sig];

	if (sa->sa_handler == EMU_SIG_IGN)
		return;

	if (sa->sa_handler == EMU_SIG_DFL) {
		/* Default actions. */
		if (sig_default_ignore(sig))
			return;

		if (sig == EMU_SIGCONT) {
			proc->state = PROC_RUNNING;
			return;
		}

		if (sig_default_stop(sig)) {
			proc->state = PROC_STOPPED;
			LOG_DBG("sig: pid %d stopped by signal %d",
			    proc->pid, sig);
			return;
		}

		/* Default: terminate. */
		LOG_DBG("sig: pid %d killed by signal %d", proc->pid, sig);
		proc_exit(proc, 128 + sig);
		return;
	}

	/*
	 * User-defined handler.
	 *
	 * Simplified delivery: save PC to X30 (link register), set PC to
	 * handler, pass signal number in X0.  A full implementation would
	 * push a sigframe on the guest stack containing all saved registers,
	 * blocked mask, and a sigreturn trampoline.
	 */
	LOG_DBG("sig: pid %d delivering signal %d to handler 0x%lx",
	    proc->pid, sig, (unsigned long)sa->sa_handler);

	proc->cpu.x[30] = proc->cpu.pc;
	proc->cpu.x[0] = (uint64_t)sig;
	proc->cpu.pc = sa->sa_handler;

	/* Block signals specified in sa_mask during handler execution. */
	proc->sig_blocked |= sa->sa_mask;
	if (!(sa->sa_flags & EMU_SA_NODEFER))
		proc->sig_blocked |= SIGMASK(sig);
	proc->sig_blocked &= ~UNCATCHABLE;

	/* SA_RESETHAND: reset to default after delivery. */
	if (sa->sa_flags & EMU_SA_RESETHAND)
		sa->sa_handler = EMU_SIG_DFL;
}
