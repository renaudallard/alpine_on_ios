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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* --- Reference-counted sigaction table ----------------------------- */

sighand_t *
sighand_create(void)
{
	sighand_t	*h;

	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return (NULL);
	h->refcount = 1;
	pthread_mutex_init(&h->lock, NULL);
	return (h);
}

sighand_t *
sighand_clone(sighand_t *src)
{
	sighand_t	*h;

	h = sighand_create();
	if (h == NULL)
		return (NULL);
	if (src != NULL) {
		pthread_mutex_lock(&src->lock);
		memcpy(h->actions, src->actions, sizeof(h->actions));
		pthread_mutex_unlock(&src->lock);
	}
	return (h);
}

sighand_t *
sighand_ref(sighand_t *h)
{
	if (h == NULL)
		return (NULL);
	pthread_mutex_lock(&h->lock);
	h->refcount++;
	pthread_mutex_unlock(&h->lock);
	return (h);
}

void
sighand_release(sighand_t *h)
{
	int	last;

	if (h == NULL)
		return;
	pthread_mutex_lock(&h->lock);
	last = (--h->refcount == 0);
	pthread_mutex_unlock(&h->lock);
	if (last) {
		pthread_mutex_destroy(&h->lock);
		free(h);
	}
}

int
sig_action(emu_process_t *proc, int sig,
    const struct emu_sigaction *act, struct emu_sigaction *oldact)
{
	sighand_t	*h;

	if (sig < 1 || sig >= EMU_NSIG)
		return (-EINVAL);

	h = proc->sighand;
	if (h == NULL)
		return (-EINVAL);

	pthread_mutex_lock(&h->lock);
	if (oldact != NULL)
		memcpy(oldact, &h->actions[sig], sizeof(*oldact));

	if (act != NULL) {
		/* Cannot change handler for SIGKILL or SIGSTOP. */
		if (sig == EMU_SIGKILL || sig == EMU_SIGSTOP) {
			pthread_mutex_unlock(&h->lock);
			return (-EINVAL);
		}
		memcpy(&h->actions[sig], act, sizeof(*act));
	}
	pthread_mutex_unlock(&h->lock);

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
	uint64_t	pending;

	pending = __atomic_load_n(&proc->sig_pending, __ATOMIC_SEQ_CST);
	*set = pending & ~proc->sig_blocked;
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

	/*
	 * Snapshot the sigaction under the sighand lock so we do not
	 * race with a concurrent sigaction() on another thread in the
	 * same group.  Everything below operates on the local copy.
	 */
	struct emu_sigaction	sa_local;

	if (proc->sighand == NULL)
		return;
	pthread_mutex_lock(&proc->sighand->lock);
	sa_local = proc->sighand->actions[sig];
	pthread_mutex_unlock(&proc->sighand->lock);
	sa = &sa_local;

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

		/* Default: terminate.  Encode as signal death for wait4:
		 * low 7 bits = signal number (WIFSIGNALED). */
		LOG_DBG("sig: pid %d killed by signal %d", proc->pid, sig);
		proc_exit(proc, sig & 0x7f);
		return;
	}

	/*
	 * User-defined handler.
	 *
	 * Push a signal frame on the guest stack containing all saved
	 * registers and the blocked signal mask.  rt_sigreturn restores
	 * this frame.
	 *
	 * Frame layout (800 bytes, 16-byte aligned):
	 *   +0:   x0-x30 (31 * 8 = 248)
	 *   +248: sp     (8)
	 *   +256: pc     (8)
	 *   +264: nzcv   (4)
	 *   +268: sig_blocked (8)
	 *   +276: pad    (4)
	 *   +280: v0-v31 (32 * 16 = 512)
	 *   +792: fpcr   (4)
	 *   +796: fpsr   (4)
	 */
#define SIGFRAME_SIZE	800

	{
		char msg[120];
		int len = snprintf(msg, sizeof(msg),
		    "[sig] pid=%d delivering sig=%d "
		    "handler=0x%lx pc=0x%lx->0x%lx\n",
		    proc->pid, sig,
		    (unsigned long)sa->handler,
		    (unsigned long)proc->cpu.pc,
		    (unsigned long)sa->handler);
		(void)write(STDERR_FILENO, msg, (size_t)len);
	}

	{
		uint64_t	frame_addr;
		int		i;

		frame_addr = (proc->cpu.sp - SIGFRAME_SIZE) & ~15ULL;

		/* Verify the frame fits in mapped memory.
		 * If not, skip delivery — the guest stack is
		 * exhausted or the SP is corrupt. */
		if (mem_translate(proc->mem, frame_addr,
		    SIGFRAME_SIZE, MEM_PROT_WRITE) == NULL) {
			LOG_WARN("sig: pid %d cannot push signal "
			    "frame at 0x%lx (sp=0x%lx)",
			    proc->pid, (unsigned long)frame_addr,
			    (unsigned long)proc->cpu.sp);
			return;
		}

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

		/* Save v0-v31, fpcr, fpsr */
		for (i = 0; i < 32; i++)
			mem_copy_to(proc->mem,
			    frame_addr + 280 + (uint64_t)i * 16,
			    &proc->cpu.v[i], 16);
		mem_write32(proc->mem, frame_addr + 792, proc->cpu.fpcr);
		mem_write32(proc->mem, frame_addr + 796, proc->cpu.fpsr);

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

	/* SA_RESETHAND: reset to default after delivery.  Write back
	 * to the shared sighand table under its lock. */
	if (sa->flags & EMU_SA_RESETHAND) {
		pthread_mutex_lock(&proc->sighand->lock);
		proc->sighand->actions[sig].handler = EMU_SIG_DFL;
		pthread_mutex_unlock(&proc->sighand->lock);
	}

#undef SIGFRAME_SIZE
}
