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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu.h"
#include "native.h"
#include "cpu.h"
#include "log.h"
#include "process.h"
#include "syscall.h"

#ifdef __aarch64__

#include <signal.h>
#include <stddef.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

/*
 * native_entry.S hardcodes field offsets from cpu_state_t.
 * If the struct layout changes (e.g. a TLB cache is added),
 * the assembly constant drifts and native_enter saves host
 * registers into the wrong field — a silent corruption that
 * crashes on process exit.  Catch it at compile time.
 */
_Static_assert(offsetof(cpu_state_t, x) == 0,
    "CPU_X0 offset changed — update native_entry.S");
_Static_assert(offsetof(cpu_state_t, sp) == 248,
    "CPU_SP offset changed — update native_entry.S");
_Static_assert(offsetof(cpu_state_t, pc) == 256,
    "CPU_PC offset changed — update native_entry.S");
_Static_assert(offsetof(cpu_state_t, nzcv) == 264,
    "CPU_NZCV offset changed — update native_entry.S");
_Static_assert(offsetof(cpu_state_t, native_host_save) == 1232,
    "CPU_HOST_SAVE offset changed — update native_entry.S");

#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#define UC_REGS(uc)	((uc)->uc_mcontext->__ss.__x)
#define UC_SP(uc)	((uc)->uc_mcontext->__ss.__sp)
#define UC_PC(uc)	((uc)->uc_mcontext->__ss.__pc)
#define UC_CPSR(uc)	((uc)->uc_mcontext->__ss.__cpsr)
#else
#include <sys/ucontext.h>
#define UC_REGS(uc)	((uc)->uc_mcontext.regs)
#define UC_SP(uc)	((uc)->uc_mcontext.sp)
#define UC_PC(uc)	((uc)->uc_mcontext.pc)
#define UC_CPSR(uc)	((uc)->uc_mcontext.pstate)
#endif

static _Thread_local emu_process_t *native_current_proc;

static void
native_sigtrap_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t	*uc;
	emu_process_t	*proc;
	uint64_t	 pc;
	uint32_t	 insn;
	uint16_t	 imm;
	int		 i, rn;

	(void)sig;
	(void)si;

	uc = (ucontext_t *)ctx;
	pc = UC_PC(uc);
	insn = *(uint32_t *)pc;
	imm = (insn >> 5) & 0xFFFF;

	proc = native_current_proc;
	if (proc == NULL) {
		/* Not in JIT context - restore default and re-raise */
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		sigaction(SIGTRAP, &sa, NULL);
		raise(SIGTRAP);
		return;
	}

	if (imm == 0x0001) {
		/*
		 * SVC #0: syscall.  Snapshot the FULL register state
		 * from uc into proc->cpu before running the handler.
		 * proc_fork memcpys proc->cpu into the child, and
		 * without the full snapshot the child would inherit
		 * stale pc/sp/x[9..30]/nzcv from the last native_exit
		 * (typically the shell's entry point), not the live
		 * post-BRK state — so the child would re-run busybox
		 * from main instead of continuing past clone and
		 * crash on the missing argc/argv/envp stack layout.
		 */
		for (i = 0; i < 31; i++)
			proc->cpu.x[i] = UC_REGS(uc)[i];
		proc->cpu.sp = UC_SP(uc);
		proc->cpu.pc = pc + 4;
		proc->cpu.nzcv = UC_CPSR(uc) & 0xF0000000;

		sys_handle(proc);

		/*
		 * Mirror the full state back to uc.  Most handlers
		 * only touch x[0] (return value), but some (signal
		 * delivery, sigreturn) rewrite x[1..30] and sp, so
		 * write everything back.
		 */
		for (i = 0; i < 31; i++)
			UC_REGS(uc)[i] = proc->cpu.x[i];
		UC_SP(uc) = proc->cpu.sp;

		if (!proc->cpu.running) {
			/* State already in proc->cpu from the pre-handler
			 * snapshot (and any handler writes above); just
			 * trampoline to native_exit. */
			UC_PC(uc) = (uint64_t)native_exit;
			UC_REGS(uc)[0] = (uint64_t)&proc->cpu;
			return;
		}
	} else if ((imm & 0xFF00) == 0x0100) {
		/* MSR TPIDR_EL0, Xn */
		rn = imm & 0x1F;
		proc->cpu.tpidr_el0 = UC_REGS(uc)[rn];
	} else if ((imm & 0xFF00) == 0x0200) {
		/* MRS Xn, TPIDR_EL0 */
		rn = imm & 0x1F;
		UC_REGS(uc)[rn] = proc->cpu.tpidr_el0;
	} else {
		/* Unknown BRK - don't use LOG_ERR (not signal-safe) */
		static const char msg[] = "native: unexpected BRK\n";
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		proc->cpu.running = 0;

		for (i = 0; i < 31; i++)
			proc->cpu.x[i] = UC_REGS(uc)[i];
		proc->cpu.sp = UC_SP(uc);
		proc->cpu.pc = pc;
		proc->cpu.nzcv = UC_CPSR(uc) & 0xF0000000;

		UC_PC(uc) = (uint64_t)native_exit;
		UC_REGS(uc)[0] = (uint64_t)&proc->cpu;
		return;
	}

	/*
	 * For SVC (imm == 1), cpu.pc was set to pc+4 by the
	 * pre-handler snapshot.  If sys_handle called execve,
	 * cpu.pc is now the new entry point.  Use cpu.pc so
	 * execve resumes at the right address.
	 *
	 * For MSR/MRS TPIDR_EL0 (imm 0x01xx/0x02xx), cpu.pc
	 * was NOT updated — use the standard pc+4.
	 */
	if (imm == 0x0001)
		UC_PC(uc) = proc->cpu.pc;
	else
		UC_PC(uc) = pc + 4;
}

int
native_init(void)
{
	struct sigaction	sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = native_sigtrap_handler;
	sa.sa_flags = SA_SIGINFO;
	sigfillset(&sa.sa_mask);

	if (sigaction(SIGTRAP, &sa, NULL) != 0) {
		LOG_ERR("native: failed to install SIGTRAP handler");
		return (-1);
	}

	LOG_INFO("native: initialized");
	return (0);
}

int
native_available(void)
{
	return (1);
}

int
native_run(emu_process_t *proc)
{
	native_current_proc = proc;
	emu_breadcrumb("native_run: about to native_enter pc=0x%lx sp=0x%lx",
	    (unsigned long)proc->cpu.pc, (unsigned long)proc->cpu.sp);
	native_enter(&proc->cpu, (void *)proc->cpu.pc);
	emu_breadcrumb("native_run: native_enter returned "
	    "(exit_code=%d)", proc->cpu.exit_code);
	/* Reached here via native_exit */
	return (proc->cpu.exit_code);
}

void
native_patch_code(void *code, size_t size)
{
	uint32_t	*insns;
	size_t		 count, i;
	uint32_t	 insn;
	int		 rn;

	insns = (uint32_t *)code;
	count = size / 4;

	for (i = 0; i < count; i++) {
		insn = insns[i];

		if (insn == 0xD4000001) {
			/* SVC #0 -> BRK #0x0001 */
			insns[i] = 0xD4200020;
		} else if ((insn & 0xFFFFFFE0) == 0xD51BD040) {
			/* MSR TPIDR_EL0, Xn -> BRK #(0x0100 | Rn) */
			rn = insn & 0x1F;
			insns[i] = 0xD4200000 | ((0x0100 | rn) << 5);
		} else if ((insn & 0xFFFFFFE0) == 0xD53BD040) {
			/* MRS Xn, TPIDR_EL0 -> BRK #(0x0200 | Rn) */
			rn = insn & 0x1F;
			insns[i] = 0xD4200000 | ((0x0200 | rn) << 5);
		}
	}

#ifdef __APPLE__
	sys_icache_invalidate(code, size);
#else
	__builtin___clear_cache(code, (char *)code + size);
#endif
}

#else /* !__aarch64__ */

int
native_init(void)
{
	LOG_INFO("native: not available (not aarch64)");
	return (0);
}

int
native_available(void)
{
	return (0);
}

int
native_run(emu_process_t *proc)
{
	(void)proc;
	LOG_ERR("native: cannot run on non-aarch64");
	return (-1);
}

void
native_patch_code(void *code, size_t size)
{
	(void)code;
	(void)size;
}

#endif /* __aarch64__ */
