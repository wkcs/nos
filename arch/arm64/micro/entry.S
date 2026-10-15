/* Copyright (C) 2026 NOS contributors */
.section .boot,"ax"
.global _start
_start:
    msr daifset, #15
    adrp x0, kernel_stack_top
    add x0, x0, :lo12:kernel_stack_top
    mov sp, x0
    mrs x1, CurrentEL
    cmp x1, #8
    b.ne 1f
    msr sp_el1, x0
    mov x1, #(1 << 31)
    msr hcr_el2, x1
    mov x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x3c5
    msr spsr_el2, x1
    adr x1, 1f
    msr elr_el2, x1
    eret
1:  adrp x0, __bss_start
    add x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
2:  cmp x0, x1
    b.hs 3f
    strb wzr, [x0], #1
    b 2b
3:  mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    adr x0, vectors
    msr vbar_el1, x0
    isb
    bl kernel_start
    b .
.section .vectors,"ax"
.balign 2048
vectors:
.macro slot target
    b \target
    .balign 128
.endm
    slot exception_fault
    slot exception_irq
    slot exception_fault
    slot exception_fault
    slot exception_fault
    slot exception_irq
    slot exception_fault
    slot exception_fault
    slot exception_sync
    slot exception_irq
    slot exception_fault
    slot exception_fault
    slot exception_fault
    slot exception_fault
    slot exception_fault
    slot exception_fault
.text
.macro save_frame
    sub sp, sp, #816
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, sp_el0
    mrs x1, elr_el1
    stp x0, x1, [sp, #248]
    mrs x0, spsr_el1
    str x0, [sp, #264]
    mrs x0, tpidr_el0
    str x0, [sp, #272]
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]
    mrs x0, fpcr
    mrs x1, fpsr
    str x0, [sp, #800]
    str x1, [sp, #808]
.endm
exception_sync:
    save_frame
    mrs x1, esr_el1
    lsr x1, x1, #26
    cmp x1, #0x15
    mov x1, #2
    b.ne handle
    mov x1, #0
    b handle
exception_irq:
    save_frame
    mov x1, #1
    b handle
exception_fault:
    save_frame
    mov x1, #2
handle:
    mov x0, sp
    bl kernel_trap
    add sp, sp, #816
    b restore
.global arch_enter_user
arch_enter_user:
restore:
    mov x30, x0
    ldr x0, [x30, #272]
    msr tpidr_el0, x0
    ldp q0, q1, [x30, #288]
    ldp q2, q3, [x30, #320]
    ldp q4, q5, [x30, #352]
    ldp q6, q7, [x30, #384]
    ldp q8, q9, [x30, #416]
    ldp q10, q11, [x30, #448]
    ldp q12, q13, [x30, #480]
    ldp q14, q15, [x30, #512]
    ldp q16, q17, [x30, #544]
    ldp q18, q19, [x30, #576]
    ldp q20, q21, [x30, #608]
    ldp q22, q23, [x30, #640]
    ldp q24, q25, [x30, #672]
    ldp q26, q27, [x30, #704]
    ldp q28, q29, [x30, #736]
    ldp q30, q31, [x30, #768]
    ldr x0, [x30, #800]
    ldr x1, [x30, #808]
    msr fpcr, x0
    msr fpsr, x1
    ldp x0, x1, [x30, #248]
    msr sp_el0, x0
    msr elr_el1, x1
    ldr x0, [x30, #264]
    msr spsr_el1, x0
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    eret
.global secondary_start
secondary_start:
    msr daifset, #15
    mrs x2, mpidr_el1
    and x2, x2, #255
    adrp x0, kernel_stack_guard
    add x0, x0, :lo12:kernel_stack_guard
    add x2, x2, #1
    add x0, x0, x2, lsl #15
    mov sp, x0
    mrs x1, CurrentEL
    cmp x1, #8
    b.ne 4f
    msr sp_el1, x0
    mov x1, #(1 << 31)
    msr hcr_el2, x1
    mov x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x3c5
    msr spsr_el2, x1
    adr x1, 4f
    msr elr_el2, x1
    eret
4:  mov x0, #(3 << 20)
    msr cpacr_el1, x0
    adr x0, vectors
    msr vbar_el1, x0
    isb
    bl kernel_secondary_start
    b .
.section .bss.stack,"aw",%nobits
.balign 4096
.global kernel_stack_guard
kernel_stack_guard:
    .space NOS_SMP * 32768
.set kernel_stack_top, kernel_stack_guard + 32768
