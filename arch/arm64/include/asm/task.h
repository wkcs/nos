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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct task_struct;

/* SVE 上限收敛至 512-bit (64 字节)，平衡端侧推理算力与内核内存预算 */
#define ARM64_SVE_MAX_VL        64U
#define ARM64_SVE_ZCR_LEN_512   3U

/* ARM64 AAPCS64 SIMD/FP 状态 + 按需扩展变长 SVE 现场指针 */
struct arm64_fp_state {
    uint64_t vregs[64];     /* V0..V31 (32 * 16B) */
    uint32_t fpsr;
    uint32_t fpcr;
    void    *sve_raw;       /* kmalloc 原始释放指针（未激活时 NULL） */
    void    *sve_state;     /* 16B 对齐后的 [Z0..Z31 | P0..P15 | FFR | FPSR/FPCR] */
    uint16_t sve_vl;        /* 当前任务生效矢量字节数（16/32/64） */
    uint8_t  valid;         /* FP/NEON 状态有效 */
    uint8_t  sve_active;    /* SVE 变长扩展已激活 */
    uint8_t  pad[4];
} __attribute__((aligned(16)));

void arm64_fp_save(struct arm64_fp_state *state);
void arm64_fp_restore(const struct arm64_fp_state *state);
void arm64_fp_zero(void);

uint32_t arm64_sve_rdvl(void);
void arm64_sve_save(void *buf, uint32_t vl);
void arm64_sve_restore(const void *buf, uint32_t vl);
void arm64_sve_zero(void);

size_t arm64_sve_state_size(uint32_t vl);
uint32_t arm64_sve_get_vl(void);
void arm64_fpu_init_cpu(void);
void arm64_fpu_trap_handler(bool is_sve);
void arm64_fpu_on_context_switch(struct task_struct *next);
void arm64_fpu_task_cleanup(struct task_struct *task);

#endif /* __ARM64_ASM_TASK_H__ */
