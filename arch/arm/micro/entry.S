/* Copyright (C) 2026 NOS contributors */
.syntax unified
.arm
.fpu neon-vfpv4
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
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(15 << 20)
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #(1 << 30)
    vmsr fpexc, r0
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
    sub sp, sp, #336
    stmia sp, {r0-r12}
    add r0, sp, #52
    stmia r0, {sp, lr}^
    nop
    str lr, [sp, #60]
    mrs r0, spsr
    str r0, [sp, #64]
    mrc p15, 0, r0, c13, c0, 2
    str r0, [sp, #68]
    add r0, sp, #72
    vstmia r0!, {d0-d15}
    vstmia r0!, {d16-d31}
    vmrs r0, fpscr
    str r0, [sp, #328]
.if \event == 2
    mrc p15, 0, r0, c0, c0, 5
    and r0, r0, #255
    ldr r1, =arm_fault_kind
    mov r2, #0
.ifc \name,prefetch_fault
    mov r2, #1
.endif
.ifc \name,data_fault
    mov r2, #2
.endif
    str r2, [r1, r0, lsl #2]
.endif
    mov r0, sp
    mov r1, #\event
    bl kernel_trap
    add sp, sp, #336
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
    mcr p15, 0, r1, c13, c0, 3
    add r1, r3, #72
    vldmia r1!, {d0-d15}
    vldmia r1!, {d16-d31}
    ldr r1, [r3, #328]
    vmsr fpscr, r1
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
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(15 << 20)
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #(1 << 30)
    vmsr fpexc, r0
    bl kernel_secondary_start
    b .
.section .bss.stack,"aw",%nobits
.balign 4096
.global kernel_stack_guard
kernel_stack_guard:
    .space NOS_SMP * 32768
.set kernel_stack_top, kernel_stack_guard + 32768

#ifdef NOS_LINUX_COMPAT
/* Linux ARM EABI helpers are fixed-address, immutable user code.  Relative
 * branches remain valid through the high user alias of this physical page. */
.section .text.kuser,"ax"
.balign 4096
.global __kuser_start
__kuser_start:
.Lkuser_cmpxchg64:
    push {r4-r7}
    ldrd r4, r5, [r0]
    ldrd r6, r7, [r1]
    dmb ish
1:  ldrexd r0, r1, [r2]
    cmp r0, r4
    cmpeq r1, r5
    bne 2f
    strexd r3, r6, r7, [r2]
    cmp r3, #0
    bne 1b
    dmb ish
    mov r0, #0
    pop {r4-r7}
    bx lr
2:  clrex
    dmb ish
    mov r0, #1
    pop {r4-r7}
    bx lr
.Lkuser_cmpxchg:
    dmb ish
1:  ldrex r3, [r2]
    cmp r3, r0
    bne 2f
    strex r3, r1, [r2]
    cmp r3, #0
    bne 1b
    dmb ish
    mov r0, #0
    cmp r0, #0
    bx lr
2:  clrex
    dmb ish
    mov r0, #1
    rsbs r0, r0, #0
    bx lr
    .space 0xf60 - (. - __kuser_start)
    b .Lkuser_cmpxchg64
    .space 0xfa0 - (. - __kuser_start)
    dmb ish
    bx lr
    .space 0xfc0 - (. - __kuser_start)
    b .Lkuser_cmpxchg
    .space 0xfe0 - (. - __kuser_start)
    mrc p15, 0, r0, c13, c0, 3
    bx lr
    .space 0xffc - (. - __kuser_start)
    .word 5
#endif
