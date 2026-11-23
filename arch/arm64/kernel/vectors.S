/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 异常向量表（严格 2KB 对齐）与 288 字节统一现场保存/恢复
 */

#define __ASSEMBLY__
#include <asm/pt_regs.h>
#include <asm/sysreg.h>

.extern arm64_sync_handler
.extern arm64_irq_dispatch
.extern arm64_irq_exit_resched
.extern arm64_serror_handler

    .macro ventry label
    .align 7
    b       \label
    .endm

    .macro save_regs from_el0
    sub     sp, sp, #PT_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30, [sp, #PT_X30]

    .if \from_el0
    mrs     x19, sp_el0
    .else
    add     x19, sp, #PT_SIZE
    .endif
    str     x19, [sp, #PT_SP]

    mrs     x20, elr_el1
    mrs     x21, spsr_el1
    stp     x20, x21, [sp, #PT_PC]

    mrs     x22, esr_el1
    mrs     x23, far_el1
    stp     x22, x23, [sp, #PT_ESR]
    .endm

    .macro restore_regs from_el0
    /* 在修改 ELR_EL1 / SPSR_EL1 前屏蔽全部异常，由 ERET 从 SPSR_EL1 原子恢复 */
    msr     daifset, #0xf
    ldp     x20, x21, [sp, #PT_PC]
    msr     elr_el1, x20
    msr     spsr_el1, x21

    .if \from_el0
    ldr     x19, [sp, #PT_SP]
    msr     sp_el0, x19
    .endif

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30, [sp, #PT_X30]
    add     sp, sp, #PT_SIZE
    eret
    .endm

    .section .vectors, "ax"
    .align 11
    .global arm64_vectors
arm64_vectors:
    /* Current EL with SP0 */
    ventry  el1_sync_invalid
    ventry  el1_irq_invalid
    ventry  el1_fiq_invalid
    ventry  el1_serror_invalid

    /* Current EL with SPx (EL1 内核态主流路径) */
    ventry  el1_sync
    ventry  el1_irq
    ventry  el1_fiq
    ventry  el1_serror

    /* Lower EL using AArch64 (EL0 用户态 / Agent Cell 主流路径) */
    ventry  el0_sync
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_serror

    /* Lower EL using AArch32 (不启用，直接异常报告) */
    ventry  el0_sync_invalid
    ventry  el0_irq_invalid
    ventry  el0_fiq_invalid
    ventry  el0_serror_invalid

el1_sync:
    save_regs 0
    mov     x0, sp
    mov     x1, #0
    bl      arm64_sync_handler
    restore_regs 0

el1_irq:
    save_regs 0
    bl      arm64_irq_dispatch
    bl      arm64_irq_exit_resched
    restore_regs 0

el1_fiq:
el1_serror:
el1_sync_invalid:
el1_irq_invalid:
el1_fiq_invalid:
el1_serror_invalid:
    save_regs 0
    mov     x0, sp
    mov     x1, #0
    bl      arm64_serror_handler
    b       .

el0_sync:
    save_regs 1
    mov     x0, sp
    mov     x1, #1
    bl      arm64_sync_handler
    bl      arm64_irq_exit_resched
    restore_regs 1

el0_irq:
    save_regs 1
    bl      arm64_irq_dispatch
    bl      arm64_irq_exit_resched
    restore_regs 1

el0_fiq:
el0_serror:
el0_sync_invalid:
el0_irq_invalid:
el0_fiq_invalid:
el0_serror_invalid:
    save_regs 1
    mov     x0, sp
    mov     x1, #1
    bl      arm64_serror_handler
    b       .

/* 首次降级进入 EL0 用户态：x0=user_pc, x1=user_sp, x2=arg0 */
    .text
    .align 2
    .global arm64_enter_user_mode
    .type arm64_enter_user_mode, %function
arm64_enter_user_mode:
    hint    #34
    msr     daifset, #0xf
    msr     elr_el1, x0
    msr     sp_el0, x1
    mov     x3, #PSR_MODE_EL0t
    msr     spsr_el1, x3
    mov     x0, x2
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret
    .size arm64_enter_user_mode, .-arm64_enter_user_mode
