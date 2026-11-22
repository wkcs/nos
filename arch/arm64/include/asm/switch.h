/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 架构上下文切换接口与切换帧契约
 */

#ifndef __ASM_ARM64_SWITCH_H__
#define __ASM_ARM64_SWITCH_H__

#include <stdint.h>
#include <kernel/types.h>

struct task_struct;

/* AAPCS64 被调用者保存寄存器切换帧（16 字节对齐，共 112 字节） */
struct arm64_switch_frame {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;          /* FP 帧指针 */
    uint64_t x30;          /* LR 返回地址 */
    uint64_t tpidr_el0;    /* 用户态 TLS 基址 */
    uint64_t reserved;     /* 维持 16 字节栈对齐 */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct arm64_switch_frame) == 112,
               "arm64_switch_frame 必须严格 16 字节对齐且为 112 字节");
#endif

/* 汇编上下文切换原语 */
void __arm64_cpu_switch_to(addr_t from_sp_ptr, addr_t to_sp_ptr);
void __arm64_cpu_switch_first(addr_t to_sp_ptr);
void arm64_task_trampoline(void);
void arm64_finish_first_switch(void);

#endif /* __ASM_ARM64_SWITCH_H__ */
