/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * Minimal Cortex-M4 Startup & Vector Table for NK60-V2 Bootloader.
 * Only the 16 architectural core vectors (64 bytes) are emitted since the
 * bootloader operates with interrupts disabled.
 */

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

    .global g_pfnVectors
    .global reset_handler

    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word reset_handler
    .word default_exception_handler /* NMI */
    .word default_exception_handler /* HardFault */
    .word default_exception_handler /* MemManage */
    .word default_exception_handler /* BusFault */
    .word default_exception_handler /* UsageFault */
    .word 0
    .word 0
    .word 0
    .word 0
    .word default_exception_handler /* SVCall */
    .word default_exception_handler /* DebugMon */
    .word 0
    .word default_exception_handler /* PendSV */
    .word default_exception_handler /* SysTick */
    .size g_pfnVectors, . - g_pfnVectors

    .section .text.reset_handler, "ax", %progbits
    .type reset_handler, %function
reset_handler:
    cpsid i
    ldr sp, =_estack

    /* Copy .data and .ramfunc from Flash (_sidata) to SRAM (_sdata.._edata) */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
1:
    cmp r0, r1
    bhs 2f
    ldr r3, [r2], #4
    str r3, [r0], #4
    b 1b

2:
    /* Zero .bss (_sbss.._ebss) */
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
3:
    cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b

4:
    bl main
5:
    b 5b
    .size reset_handler, . - reset_handler

    .section .text.default_exception_handler, "ax", %progbits
    .type default_exception_handler, %function
default_exception_handler:
    b default_exception_handler
    .size default_exception_handler, . - default_exception_handler
