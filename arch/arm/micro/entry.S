/* Copyright (C) 2026 NOS contributors */
.syntax unified
.arm
.section .boot,"ax"
.global _start
_start:
    cpsid if
    cps #0x12
    ldr sp, =kernel_stack_top
    cps #0x17
    ldr sp, =kernel_stack_top
    cps #0x1b
    ldr sp, =kernel_stack_top
    cps #0x13
    ldr sp, =kernel_stack_top
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strblo r2, [r0], #1
    blo 1b
    ldr r0, =vectors
    mcr p15, 0, r0, c12, c0, 0
    isb
    bl kernel_start
    b .
.section .vectors,"ax"
.balign 32
vectors:
    b _start
    b undefined
    b syscall
    b prefetch_fault
    b data_fault
    b undefined
    b interrupt
    b undefined
.text
.macro trap name, offset, event
\name:
    sub lr, lr, #\offset
    sub sp, sp, #72
    stmia sp, {r0-r12}
    add r0, sp, #52
    stmia r0, {sp, lr}^
    nop
    str lr, [sp, #60]
    mrs r0, spsr
    str r0, [sp, #64]
    mrc p15, 0, r0, c13, c0, 2
    str r0, [sp, #68]
    mov r0, sp
    mov r1, #\event
    bl kernel_trap
    add sp, sp, #72
    b restore
.endm
trap syscall, 0, 0
trap interrupt, 4, 1
trap undefined, 4, 2
trap prefetch_fault, 4, 2
trap data_fault, 8, 2
.global arch_enter_user
arch_enter_user:
restore:
    mov r3, r0
    ldr r1, [r3, #68]
    mcr p15, 0, r1, c13, c0, 2
    ldr r1, [r3, #64]
    msr spsr_cxsf, r1
    add r1, r3, #52
    ldmia r1, {sp, lr}^
    nop
    ldr lr, [r3, #60]
    ldmia r3, {r0-r12}
    movs pc, lr
.global secondary_start
secondary_start:
    cpsid if
    mrc p15, 0, r0, c0, c0, 5
    and r0, r0, #255
    add r0, r0, #1
    ldr r1, =kernel_stack_guard
    add r1, r1, r0, lsl #15
    cps #0x12
    mov sp, r1
    cps #0x17
    mov sp, r1
    cps #0x1b
    mov sp, r1
    cps #0x13
    mov sp, r1
    ldr r0, =vectors
    mcr p15, 0, r0, c12, c0, 0
    isb
    bl kernel_secondary_start
    b .
.section .bss.stack,"aw",%nobits
.balign 4096
.global kernel_stack_guard
kernel_stack_guard:
    .space NOS_SMP * 32768
.set kernel_stack_top, kernel_stack_guard + 32768
