/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Per-CPU 运行时状态（绑定至 TPIDR_EL1，O(1) 无锁查询）
 */

#ifndef __ASM_ARM64_PERCPU_H__
#define __ASM_ARM64_PERCPU_H__

#include <stdint.h>
#include <stdbool.h>
#include <uapi/nos/types.h>

#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

struct task_struct;

struct arm64_percpu {
    uint32_t            cpu_id;
    uint64_t            mpidr;
    uintptr_t           irq_stack_top;
    uint32_t            cpu_capacity;       /* big.LITTLE EAS 算力权重 (默认 1024) */
    bool                deferred_switch;    /* ISR 内部请求的延迟抢占标记 */
    addr_t              deferred_from;      /* 原任务 sp 指针地址 */
    addr_t              deferred_to;        /* 目标任务 sp 指针地址 */
    struct task_struct *fpu_owner;          /* 本核当前向量寄存器所属任务 */
    bool                online;
};

void arm64_percpu_init(uint32_t cpu_id);
struct arm64_percpu *arm64_this_percpu(void);
struct arm64_percpu *arm64_percpu_of(uint32_t cpu_id);

#endif /* __ASM_ARM64_PERCPU_H__ */
