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

#include "jit.h"
#include "cpu.h"
#include "log.h"
#include "process.h"
#include "syscall.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <dlfcn.h>

/*
 * pthread_jit_write_protect_np is available at runtime on iOS 14.2+
 * and macOS 11+ but iOS SDK headers mark it __API_UNAVAILABLE(ios).
 * Resolve via dlsym to bypass the header restriction.
 */
static void (*jit_write_protect_fn)(int);

void
jit_write_protect(int enabled)
{
	if (jit_write_protect_fn != NULL)
		jit_write_protect_fn(enabled);
}
#endif

#ifdef __aarch64__

#include <signal.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

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

static _Thread_local emu_process_t *jit_current_proc;

static void
jit_sigtrap_handler(int sig, siginfo_t *si, void *ctx)
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

	proc = jit_current_proc;
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
		/* SVC #0: syscall */
		proc->cpu.x[8] = UC_REGS(uc)[8];
		for (i = 0; i < 6; i++)
			proc->cpu.x[i] = UC_REGS(uc)[i];

		sys_handle(proc);

		/* Write back return value */
		UC_REGS(uc)[0] = proc->cpu.x[0];

		if (!proc->cpu.running) {
			/* Save guest state and jump to jit_exit */
			for (i = 0; i < 31; i++)
				proc->cpu.x[i] = UC_REGS(uc)[i];
			proc->cpu.sp = UC_SP(uc);
			proc->cpu.pc = pc + 4;
			proc->cpu.nzcv = UC_CPSR(uc) & 0xF0000000;

			UC_PC(uc) = (uint64_t)jit_exit;
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
		/* Unknown BRK */
		LOG_ERR("jit: unexpected BRK #0x%x at 0x%llx", imm,
		    (unsigned long long)pc);
		proc->cpu.running = 0;

		for (i = 0; i < 31; i++)
			proc->cpu.x[i] = UC_REGS(uc)[i];
		proc->cpu.sp = UC_SP(uc);
		proc->cpu.pc = pc;
		proc->cpu.nzcv = UC_CPSR(uc) & 0xF0000000;

		UC_PC(uc) = (uint64_t)jit_exit;
		UC_REGS(uc)[0] = (uint64_t)&proc->cpu;
		return;
	}

	UC_PC(uc) = pc + 4;
}

int
jit_init(void)
{
	struct sigaction	sa;

#ifdef __APPLE__
	/* Resolve pthread_jit_write_protect_np at runtime */
	jit_write_protect_fn = (void (*)(int))
	    dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np");
	if (jit_write_protect_fn != NULL)
		LOG_INFO("jit: pthread_jit_write_protect_np available");
	else
		LOG_WARN("jit: pthread_jit_write_protect_np not found");
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = jit_sigtrap_handler;
	sa.sa_flags = SA_SIGINFO;
	sigfillset(&sa.sa_mask);

	if (sigaction(SIGTRAP, &sa, NULL) != 0) {
		LOG_ERR("jit: failed to install SIGTRAP handler");
		return (-1);
	}

	LOG_INFO("jit: initialized");
	return (0);
}

int
jit_available(void)
{
	return (1);
}

int
jit_run(emu_process_t *proc)
{
	jit_current_proc = proc;

	/* In JIT mode, guest addr = host addr */
	jit_enter(&proc->cpu, (void *)proc->cpu.pc);

	/* Reached here via jit_exit */
	return (proc->cpu.exit_code);
}

void
jit_patch_code(void *code, size_t size)
{
	uint32_t	*insns;
	size_t		 count, i;
	uint32_t	 insn;
	int		 rn;

	insns = (uint32_t *)code;
	count = size / 4;

#ifdef __APPLE__
	JIT_WRITE_ENABLE();
#endif

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
	JIT_WRITE_DISABLE();
#endif

#ifdef __APPLE__
	sys_icache_invalidate(code, size);
#else
	__builtin___clear_cache(code, (char *)code + size);
#endif
}

#else /* !__aarch64__ */

int
jit_init(void)
{
	LOG_INFO("jit: not available (not aarch64)");
	return (0);
}

int
jit_available(void)
{
	return (0);
}

int
jit_run(emu_process_t *proc)
{
	(void)proc;
	LOG_ERR("jit: cannot run on non-aarch64");
	return (-1);
}

void
jit_patch_code(void *code, size_t size)
{
	(void)code;
	(void)size;
}

#endif /* __aarch64__ */
