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

	__atomic_or_fetch(&proc->sig_pending, SIGMASK(sig), __ATOMIC_SEQ_CST);
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

	deliverable = __atomic_load_n(&proc->sig_pending, __ATOMIC_SEQ_CST)
	    & ~proc->sig_blocked;
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
	__atomic_and_fetch(&proc->sig_pending, ~SIGMASK(sig),
	    __ATOMIC_SEQ_CST);

	sa = &proc->sigactions[sig];

	if (sa->handler == EMU_SIG_IGN)
		return;

	if (sa->handler == EMU_SIG_DFL) {
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
	 * Push a signal frame on the guest stack containing all saved
	 * registers and the blocked signal mask.  rt_sigreturn restores
	 * this frame.
	 *
	 * Frame layout (288 bytes, 16-byte aligned):
	 *   +0:   x0-x30 (31 * 8 = 248)
	 *   +248: sp     (8)
	 *   +256: pc     (8)
	 *   +264: nzcv   (4)
	 *   +268: sig_blocked (8)
	 */
#define SIGFRAME_SIZE	288

	LOG_DBG("sig: pid %d delivering signal %d to handler 0x%lx",
	    proc->pid, sig, (unsigned long)sa->handler);

	{
		uint64_t	frame_addr;
		int		i;

		frame_addr = (proc->cpu.sp - SIGFRAME_SIZE) & ~15ULL;

		/* Save x0-x30 */
		for (i = 0; i < 31; i++)
			mem_write64(proc->mem,
			    frame_addr + (uint64_t)i * 8, proc->cpu.x[i]);

		/* Save sp, pc, nzcv, blocked mask */
		mem_write64(proc->mem, frame_addr + 248, proc->cpu.sp);
		mem_write64(proc->mem, frame_addr + 256, proc->cpu.pc);
		mem_write32(proc->mem, frame_addr + 264, proc->cpu.nzcv);
		mem_write64(proc->mem, frame_addr + 268,
		    proc->sig_blocked);

		/* Set up for handler execution */
		proc->cpu.sp = frame_addr;
		proc->cpu.x[0] = (uint64_t)sig;
		proc->cpu.pc = sa->handler;

		/*
		 * If SA_RESTORER is set, use the restorer as the return
		 * address.  Otherwise set X30 to 0 (handler must call
		 * rt_sigreturn itself).
		 */
		if (sa->flags & EMU_SA_RESTORER)
			proc->cpu.x[30] = sa->restorer;
		else
			proc->cpu.x[30] = 0;
	}

	/* Block signals specified in sa_mask during handler execution. */
	proc->sig_blocked |= sa->mask;
	if (!(sa->flags & EMU_SA_NODEFER))
		proc->sig_blocked |= SIGMASK(sig);
	proc->sig_blocked &= ~UNCATCHABLE;

	/* SA_RESETHAND: reset to default after delivery. */
	if (sa->flags & EMU_SA_RESETHAND)
		sa->handler = EMU_SIG_DFL;

#undef SIGFRAME_SIZE
}
