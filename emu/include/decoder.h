/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef DECODER_H
#define DECODER_H

#include "cpu.h"

/*
 * Instruction group handlers.
 * Each returns EMU_OK on success, EMU_SYSCALL on SVC, or an error code.
 */
int	exec_dp_imm(cpu_state_t *cpu, uint32_t insn);
int	exec_dp_reg(cpu_state_t *cpu, uint32_t insn);
int	exec_ldst(cpu_state_t *cpu, uint32_t insn);
int	exec_branch(cpu_state_t *cpu, uint32_t insn);
int	exec_system(cpu_state_t *cpu, uint32_t insn);
int	exec_simd(cpu_state_t *cpu, uint32_t insn);

/* Shift and extend helpers (defined in decoder.c) */
uint64_t	apply_shift(uint64_t val, int shift_type, int amount);
uint32_t	apply_shift32(uint32_t val, int shift_type, int amount);
uint64_t	extend_reg(cpu_state_t *cpu, int reg, int ext_type, int shift);

#endif /* DECODER_H */
