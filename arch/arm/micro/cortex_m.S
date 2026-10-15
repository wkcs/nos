/* Copyright (C) 2026 NOS contributors */
.syntax unified
.thumb
.section .vectors,"a"
.global _start
_start:
    .word __stack_top
    .word reset_handler
    .word fault_handler
    .word fault_handler
    .word fault_handler
    .word fault_handler
    .word fault_handler
    .word 0, 0, 0, 0
    .word svc_handler
    .word fault_handler
    .word 0
    .word fault_handler
    .word timer_handler
    .rept 37
    .word fault_handler
    .endr
    .word external_handler
.section .boot,"ax"
.thumb_func
reset_handler:
    cpsid i
    ldr r0, =__data_start
    ldr r1, =__data_end
    ldr r2, =__data_load
1:  cmp r0, r1
    bhs 2f
    ldrb r3, [r2], #1
    strb r3, [r0], #1
    b 1b
2:  ldr r0, =__bss_start
    ldr r1, =__bss_end
    movs r2, #0
3:  cmp r0, r1
    bhs 4f
    strb r2, [r0], #1
    b 3b
4:  ldr r0, =__user_data_start
    ldr r1, =__user_data_end
    ldr r2, =__user_data_load
5:  cmp r0, r1
    bhs 6f
    ldrb r3, [r2], #1
    strb r3, [r0], #1
    b 5b
6:  ldr r0, =__user_bss_start
    ldr r1, =__user_bss_end
    movs r2, #0
7:  cmp r0, r1
    bhs 8f
    strb r2, [r0], #1
    b 7b
8:  bl kernel_start
    b .
.text
.thumb_func
svc_handler:
    tst lr, #4
    beq first_return
    movs r1, #0
    b trap_handler
.thumb_func
timer_handler:
    movs r1, #1
    b trap_handler
.thumb_func
external_handler:
    movs r1, #3
    b trap_handler
.thumb_func
fault_handler:
    /* EXC_RETURN bit 2 distinguishes a user PSP frame from a kernel MSP
     * frame.  A kernel fault must never be attributed to the current user. */
    tst lr, #4
    beq kernel_fault
    movs r1, #2
trap_handler:
    mrs r0, psp
    stmdb r0!, {r4-r11}
    push {r3, lr}
    bl kernel_trap
    pop {r3, lr}
    b restore
kernel_fault:
    ldr r0, =kernel_fault_message
    bl kernel_fail
    b .
.section .rodata
kernel_fault_message:
    .asciz "privileged_exception"
.text
.thumb_func
first_return:
    ldr r0, =first_frame
    ldr r0, [r0]
    ldr lr, =0xfffffffd
restore:
    ldmia r0!, {r4-r11}
    msr psp, r0
    movs r0, #3
    msr control, r0
    isb
    bx lr
.global arch_enter_user
.thumb_func
arch_enter_user:
    ldr r1, =first_frame
    str r0, [r1]
    cpsie i
    svc #255
    b .
.section .bss,"aw",%nobits
.balign 4
first_frame:
    .space 4
