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

#include <string.h>
#include <sys/types.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "signal_emu.h"
#include "log.h"

/* Linux errno */
#define LINUX_EINVAL	22
#define LINUX_EFAULT	14
#define LINUX_ESRCH	3
#define LINUX_ENOSYS	38

/* Linux SIG_BLOCK/UNBLOCK/SETMASK */
#define LINUX_SIG_BLOCK		0
#define LINUX_SIG_UNBLOCK	1
#define LINUX_SIG_SETMASK	2

static int64_t
do_rt_sigaction(emu_process_t *proc, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
	int			sig;
	struct emu_sigaction	act, oldact;

	(void)a3;	/* sigsetsize */

	sig = (int)a0;
	if (sig < 1 || sig >= EMU_NSIG)
		return -LINUX_EINVAL;

	/* Read new action if provided. */
	if (a1 != 0) {
		if (mem_read64(proc->mem, a1, &act.handler) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a1 + 8, &act.flags) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a1 + 16, &act.restorer) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a1 + 24, &act.mask) != 0)
			return -LINUX_EFAULT;
	}

	if (sig_action(proc, sig, a1 ? &act : NULL,
	    a2 ? &oldact : NULL) != 0)
		return -LINUX_EINVAL;

	/* Write old action if requested. */
	if (a2 != 0) {
		if (mem_write64(proc->mem, a2, oldact.handler) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a2 + 8, oldact.flags) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a2 + 16, oldact.restorer) != 0)
			return -LINUX_EFAULT;
		if (mem_write64(proc->mem, a2 + 24, oldact.mask) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_rt_sigprocmask(emu_process_t *proc, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
	int		how;
	uint64_t	set, oldset;

	(void)a3;	/* sigsetsize */

	how = (int)a0;

	if (a1 != 0) {
		if (mem_read64(proc->mem, a1, &set) != 0)
			return -LINUX_EFAULT;
	}

	if (sig_procmask(proc, how, a1 ? &set : NULL,
	    a2 ? &oldset : NULL) != 0)
		return -LINUX_EINVAL;

	if (a2 != 0) {
		if (mem_write64(proc->mem, a2, oldset) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_rt_sigpending(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	uint64_t	pending;

	(void)a1;	/* sigsetsize */

	if (sig_pending(proc, &pending) != 0)
		return -LINUX_EINVAL;

	if (a0 != 0) {
		if (mem_write64(proc->mem, a0, pending) != 0)
			return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_kill(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		pid, sig;
	emu_process_t	*target;

	pid = (int)(int32_t)a0;
	sig = (int)a1;

	if (sig == 0) {
		/* Signal 0: check process existence. */
		if (pid > 0) {
			target = proc_find(pid);
			if (target == NULL)
				return -LINUX_ESRCH;
		}
		return 0;
	}

	if (pid > 0) {
		target = proc_find(pid);
		if (target == NULL)
			return -LINUX_ESRCH;
		return sig_send(target, sig);
	}

	if (pid == 0 || pid == -1) {
		/* Send to own process group or all processes. */
		return sig_send(proc, sig);
	}

	/* pid < -1: send to process group |pid|. */
	return sig_send(proc, sig);
}

static int64_t
do_tkill(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		tid, sig;
	emu_process_t	*target;

	tid = (int)(int32_t)a0;
	sig = (int)a1;

	(void)proc;

	target = proc_find(tid);
	if (target == NULL)
		return -LINUX_ESRCH;

	return sig_send(target, sig);
}

static int64_t
do_tgkill(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	(void)a0;	/* tgid, ignored */
	return do_tkill(proc, a1, a2);
}

static int64_t
do_sigaltstack(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	/* Write old stack if requested. */
	if (a1 != 0) {
		/* struct sigaltstack: ss_sp(8), ss_flags(4), pad(4), ss_size(8) */
		mem_write64(proc->mem, a1,
		    (uint64_t)(uintptr_t)proc->sig_altstack);
		mem_write32(proc->mem, a1 + 8,
		    proc->sig_altstack != NULL ? 0 : 2);	/* SS_DISABLE=2 */
		mem_write64(proc->mem, a1 + 16,
		    (uint64_t)proc->sig_altstack_size);
	}

	/* Set new stack if provided. */
	if (a0 != 0) {
		uint64_t	sp;
		uint32_t	flags;
		uint64_t	size;

		if (mem_read64(proc->mem, a0, &sp) != 0)
			return -LINUX_EFAULT;
		if (mem_read32(proc->mem, a0 + 8, &flags) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a0 + 16, &size) != 0)
			return -LINUX_EFAULT;

		if (flags & 2) {	/* SS_DISABLE */
			proc->sig_altstack = NULL;
			proc->sig_altstack_size = 0;
		} else {
			proc->sig_altstack = (void *)(uintptr_t)sp;
			proc->sig_altstack_size = (size_t)size;
		}
	}

	return 0;
}

/*
 * Restore registers from signal frame on the guest stack.
 *
 * The frame layout (pushed by sig_deliver in signal_emu.c):
 *   sp+0:   x0-x30 (31 * 8 = 248 bytes)
 *   sp+248: sp     (8 bytes)
 *   sp+256: pc     (8 bytes)
 *   sp+264: nzcv   (4 bytes)
 *   sp+268: sig_blocked (8 bytes)
 *
 * Total: 276 bytes, rounded up to 288 for alignment.
 */
#define SIGFRAME_SIZE	288

static int64_t
do_rt_sigreturn(emu_process_t *proc)
{
	uint64_t	frame_addr, val;
	uint32_t	nzcv;
	int		i;

	frame_addr = proc->cpu.sp;

	/* Restore x0-x30 */
	for (i = 0; i < 31; i++) {
		if (mem_read64(proc->mem, frame_addr + (uint64_t)i * 8,
		    &val) != 0)
			return -LINUX_EFAULT;
		proc->cpu.x[i] = val;
	}

	/* Restore sp */
	if (mem_read64(proc->mem, frame_addr + 248, &val) != 0)
		return -LINUX_EFAULT;
	proc->cpu.sp = val;

	/* Restore pc */
	if (mem_read64(proc->mem, frame_addr + 256, &val) != 0)
		return -LINUX_EFAULT;
	proc->cpu.pc = val;

	/* Restore nzcv */
	if (mem_read32(proc->mem, frame_addr + 264, &nzcv) != 0)
		return -LINUX_EFAULT;
	proc->cpu.nzcv = nzcv;

	/* Restore blocked signal mask */
	if (mem_read64(proc->mem, frame_addr + 268, &val) != 0)
		return -LINUX_EFAULT;
	proc->sig_blocked = val;

	/*
	 * Return value is already in x0 from the restored frame.
	 * The caller (sys_handle) would overwrite x0, so we return the
	 * restored x0 value directly.
	 */
	return (int64_t)proc->cpu.x[0];
}

int64_t
sys_signal(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	(void)a4;
	(void)a5;

	switch (nr) {
	case SYS_RT_SIGACTION:
		return do_rt_sigaction(proc, a0, a1, a2, a3);
	case SYS_RT_SIGPROCMASK:
		return do_rt_sigprocmask(proc, a0, a1, a2, a3);
	case SYS_RT_SIGPENDING:
		return do_rt_sigpending(proc, a0, a1);
	case SYS_RT_SIGRETURN:
		return do_rt_sigreturn(proc);
	case SYS_RT_SIGSUSPEND:
		/* Stub: temporarily replace mask and pause. */
		return 0;
	case SYS_RT_SIGTIMEDWAIT:
		/* Stub: return 0. */
		return 0;
	case SYS_KILL:
		return do_kill(proc, a0, a1);
	case SYS_TKILL:
		return do_tkill(proc, a0, a1);
	case SYS_TGKILL:
		return do_tgkill(proc, a0, a1, a2);
	case SYS_SIGALTSTACK:
		return do_sigaltstack(proc, a0, a1);
	default:
		LOG_WARN("sys_signal: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
