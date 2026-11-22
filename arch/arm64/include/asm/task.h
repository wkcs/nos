/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM64_ASM_TASK_H__
#define __ARM64_ASM_TASK_H__

#include <kernel/kernel.h>
#include <stdint.h>

/* ARM64 AAPCS64 SIMD/FP 状态（32×128-bit Q0..Q31 + FPSR/FPCR，16B 对齐共 528B） */
struct arm64_fp_state {
    uint64_t vregs[64];
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t  valid;
    uint8_t  pad[7];
} __attribute__((aligned(16)));

void arm64_fp_save(struct arm64_fp_state *state);
void arm64_fp_restore(const struct arm64_fp_state *state);
void arm64_fp_zero(void);

#endif /* __ARM64_ASM_TASK_H__ */
