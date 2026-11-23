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
    hint    #34
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
    hint    #34
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
    hint    #34
    /* x19 = entry, x20 = parameter, x21 = task_exit（callee-saved 在函数调用后不变） */
    bl      arm64_finish_first_switch
    mov     x0, x20
    blr     x19
    blr     x21
1:
    wfi
    b       1b
    .size arm64_task_trampoline, .-arm64_task_trampoline

    /*
     * ARM64 惰性 NEON/FP 向量寄存器保存/恢复原语
     * x0 = struct arm64_fp_state *（vregs[64] @ 0..511, fpsr @ 512, fpcr @ 516）
     */
    .global arm64_fp_save
    .type arm64_fp_save, %function
arm64_fp_save:
    hint    #34
    stp     q0,  q1,  [x0, #0]
    stp     q2,  q3,  [x0, #32]
    stp     q4,  q5,  [x0, #64]
    stp     q6,  q7,  [x0, #96]
    stp     q8,  q9,  [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x8, fpsr
    str     w8, [x0, #512]
    mrs     x8, fpcr
    str     w8, [x0, #516]
    ret
    .size arm64_fp_save, .-arm64_fp_save

    .global arm64_fp_restore
    .type arm64_fp_restore, %function
arm64_fp_restore:
    hint    #34
    ldp     q0,  q1,  [x0, #0]
    ldp     q2,  q3,  [x0, #32]
    ldp     q4,  q5,  [x0, #64]
    ldp     q6,  q7,  [x0, #96]
    ldp     q8,  q9,  [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    ldr     w8, [x0, #512]
    msr     fpsr, x8
    ldr     w8, [x0, #516]
    msr     fpcr, x8
    ret
    .size arm64_fp_restore, .-arm64_fp_restore

    .global arm64_fp_zero
    .type arm64_fp_zero, %function
arm64_fp_zero:
    hint    #34
    movi    v0.2d,  #0
    movi    v1.2d,  #0
    movi    v2.2d,  #0
    movi    v3.2d,  #0
    movi    v4.2d,  #0
    movi    v5.2d,  #0
    movi    v6.2d,  #0
    movi    v7.2d,  #0
    movi    v8.2d,  #0
    movi    v9.2d,  #0
    movi    v10.2d, #0
    movi    v11.2d, #0
    movi    v12.2d, #0
    movi    v13.2d, #0
    movi    v14.2d, #0
    movi    v15.2d, #0
    movi    v16.2d, #0
    movi    v17.2d, #0
    movi    v18.2d, #0
    movi    v19.2d, #0
    movi    v20.2d, #0
    movi    v21.2d, #0
    movi    v22.2d, #0
    movi    v23.2d, #0
    movi    v24.2d, #0
    movi    v25.2d, #0
    movi    v26.2d, #0
    movi    v27.2d, #0
    movi    v28.2d, #0
    movi    v29.2d, #0
    movi    v30.2d, #0
    movi    v31.2d, #0
    msr     fpsr, xzr
    msr     fpcr, xzr
    ret
    .size arm64_fp_zero, .-arm64_fp_zero
