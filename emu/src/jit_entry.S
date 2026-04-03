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

#ifdef __aarch64__

/* cpu_state_t field offsets (computed from struct layout) */
#define CPU_X0		0
#define CPU_SP		248
#define CPU_PC		256
#define CPU_NZCV	264
#define CPU_HOST_SAVE	848

/*
 * void jit_enter(cpu_state_t *cpu, void *host_pc)
 *
 * x0 = pointer to cpu_state_t
 * x1 = host PC to jump to (guest code address)
 *
 * Saves host callee-saved registers, loads guest state, branches to
 * guest code.  Returns when jit_exit is invoked by the signal handler.
 */

#ifdef __APPLE__
	.globl _jit_enter
	.globl _jit_exit
	.p2align 2
_jit_enter:
#else
	.globl jit_enter
	.globl jit_exit
	.p2align 2
	.type jit_enter, %function
jit_enter:
#endif

	/*
	 * Save host callee-saved registers into cpu->jit_host_save.
	 * The offset (848) is too large for stp immediate, so
	 * compute the save area address in x16 first.
	 */
	add	x16, x0, #CPU_HOST_SAVE
	stp	x19, x20, [x16, #0]
	stp	x21, x22, [x16, #16]
	stp	x23, x24, [x16, #32]
	stp	x25, x26, [x16, #48]
	stp	x27, x28, [x16, #64]
	stp	x29, x30, [x16, #80]
	mov	x17, sp
	str	x17, [x16, #96]

	/* x0 = cpu, x1 = target PC */
	mov	x17, x0
	mov	x16, x1

	/* Load guest NZCV */
	ldr	w15, [x17, #CPU_NZCV]
	msr	nzcv, x15

	/* Load guest SP */
	ldr	x15, [x17, #CPU_SP]
	mov	sp, x15

	/* Load guest x0-x15 */
	ldp	x0,  x1,  [x17, #(CPU_X0 + 0)]
	ldp	x2,  x3,  [x17, #(CPU_X0 + 16)]
	ldp	x4,  x5,  [x17, #(CPU_X0 + 32)]
	ldp	x6,  x7,  [x17, #(CPU_X0 + 48)]
	ldp	x8,  x9,  [x17, #(CPU_X0 + 64)]
	ldp	x10, x11, [x17, #(CPU_X0 + 80)]
	ldp	x12, x13, [x17, #(CPU_X0 + 96)]
	ldp	x14, x15, [x17, #(CPU_X0 + 112)]

	/* Load guest x18-x28 */
	ldr	x18, [x17, #(CPU_X0 + 144)]
	ldp	x19, x20, [x17, #(CPU_X0 + 152)]
	ldp	x21, x22, [x17, #(CPU_X0 + 168)]
	ldp	x23, x24, [x17, #(CPU_X0 + 184)]
	ldp	x25, x26, [x17, #(CPU_X0 + 200)]
	ldp	x27, x28, [x17, #(CPU_X0 + 216)]

	/* Load guest x29, x30 */
	ldp	x29, x30, [x17, #(CPU_X0 + 232)]

	/* Jump to guest code (x16 has target, x17 is clobbered) */
	br	x16

/*
 * jit_exit:
 *
 * Jumped to by the signal handler via ucontext PC modification.
 * x0 = pointer to cpu_state_t (set by signal handler).
 *
 * Restores host callee-saved registers, SP, and returns to
 * jit_enter's caller.
 */
#ifdef __APPLE__
	.p2align 2
_jit_exit:
#else
	.p2align 2
	.type jit_exit, %function
jit_exit:
#endif

	/* Compute save area address */
	add	x16, x0, #CPU_HOST_SAVE

	/* Restore host SP */
	ldr	x17, [x16, #96]
	mov	sp, x17

	/* Restore host callee-saved registers */
	ldp	x19, x20, [x16, #0]
	ldp	x21, x22, [x16, #16]
	ldp	x23, x24, [x16, #32]
	ldp	x25, x26, [x16, #48]
	ldp	x27, x28, [x16, #64]
	ldp	x29, x30, [x16, #80]

	/* Return 0 to jit_enter's caller */
	mov	x0, #0
	ret

#endif /* __aarch64__ */
