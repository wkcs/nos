/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 AAPCS64 上下文切换与新任务首启蹦床汇编实现
 * 栈帧结构严格对应 struct arm64_switch_frame (112 字节 16B 对齐)
 */

    .text
    .align 2

    .global __arm64_cpu_switch_to
    .type __arm64_cpu_switch_to, %function
__arm64_cpu_switch_to:
    /* x0 = &from_task->sp, x1 = &to_task->sp */
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mrs     x8, tpidr_el0
    str     x8, [sp, #96]
    str     xzr, [sp, #104]

    mov     x9, sp
    str     x9, [x0]

    ldr     x9, [x1]
    mov     sp, x9
    ldr     x8, [sp, #96]
    msr     tpidr_el0, x8
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret
    .size __arm64_cpu_switch_to, .-__arm64_cpu_switch_to

    .global __arm64_cpu_switch_first
    .type __arm64_cpu_switch_first, %function
__arm64_cpu_switch_first:
    /* x0 = &to_task->sp */
    ldr     x9, [x0]
    mov     sp, x9
    ldr     x8, [sp, #96]
    msr     tpidr_el0, x8
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret
    .size __arm64_cpu_switch_first, .-__arm64_cpu_switch_first

    .extern arm64_finish_first_switch
    .global arm64_task_trampoline
    .type arm64_task_trampoline, %function
arm64_task_trampoline:
    /* x19 = entry, x20 = parameter, x21 = task_exit（callee-saved 在函数调用后不变） */
    bl      arm64_finish_first_switch
    mov     x0, x20
    blr     x19
    blr     x21
1:
    wfi
    b       1b
    .size arm64_task_trampoline, .-arm64_task_trampoline
