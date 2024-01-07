/*
 * Copyright (C) 2018 胡启航<Hu Qihang>
 *
 * Author: wkcs
 *
 * Email: hqh2030@gmail.com, huqihan@live.com
 */

    .cpu    cortex-m3
    .fpu    softvfp
    .syntax unified
    .thumb
    .text

    .global switch_interrupt_flag
    .global interrupt_from_task
    .global interrupt_to_task
    .global kernel_running
    .global interrupt_nest

    .equ    SCB_VTOR, 0xE000ED08            /* Vector Table Offset Register */
    .equ    ICSR, 0xE000ED04                /* interrupt control state register */
    .equ    PENDSVSET_BIT, 0x10000000       /* value to trigger PendSV exception */

    .equ    SHPR3, 0xE000ED20               /* system priority register (3) */
    .equ    PENDSV_PRI_LOWEST, 0x00FF0000   /* PendSV priority value (lowest) */

/*
 * addr_t asm_disable_irq_save();
 */
    .global asm_disable_irq_save
    .type asm_disable_irq_save, %function
asm_disable_irq_save:
    mrs     r0, primask
    cpsid   i
    bx      lr

/*
 * void asm_enable_irq_save(addr_t level);
 */
    .global asm_enable_irq_save
    .type asm_enable_irq_save, %function
asm_enable_irq_save:
    msr     primask, r0
    bx      lr

    .global context_switch_interrupt
    .type context_switch_interrupt, %function
    .global context_switch
    .type context_switch, %function
context_switch_interrupt:
context_switch:
    /* set switch_interrupt_flag to 1 */
    ldr     r2, =switch_interrupt_flag
    ldr     r3, [r2]
    cmp     r3, #1
    beq     _reswitch
    mov     r3, #1
    str     r3, [r2]

    ldr     r2, =interrupt_from_task
    str     r0, [r2]

_reswitch:
    ldr     r2, =interrupt_to_task
    str     r1, [r2]

    ldr     r0, =ICSR
    ldr     r1, =PENDSVSET_BIT
    str     r1, [r0]
    bx      lr

/* r0 --> switch from task stack
 * r1 --> switch to task stack
 * psr, pc, lr, R12, r3, r2, r1, r0 are pushed into [from] stack
 */
    .global PendSV_Handler
    .type PendSV_Handler, %function
PendSV_Handler:
    /* disable interrupt to protect context switch */
    mrs     r2, primask
    cpsid   i

    /* get switch_interrupt_flag */
    ldr     r0, =switch_interrupt_flag
    ldr     r1, [r0]
    cbz     r1, pendsv_exit         /* pendsv already handled */

    /* clear switch_interrupt_flag to 0 */
    mov     r1, #0
    str     r1, [r0]

    ldr     r0, =interrupt_from_task
    ldr     r1, [r0]
    cbz     r1, switch_to_task      /* skip register save at the first time */

    mrs     r1, psp                 /* get from task stack pointer */
    stmfd   r1!, {r4 - r11}         /* push r4 - r11 register */
    ldr     r0, [r0]
    str     r1, [r0]                /* update from task stack pointer */

switch_to_task:
    ldr     r1, =interrupt_to_task
    ldr     r1, [r1]
    ldr     r1, [r1]                /* load task stack pointer */

    ldmfd   r1!, {r4 - r11}         /* pop r4 - r11 register */
    msr     psp, r1                 /* update stack pointer */

pendsv_exit:
    /* restore interrupt */
    msr     primask, r2
    orr     lr, lr, #0x04
    bx      lr

/*
 * void context_switch_to(addr_t to);
 * r0 --> to
 */
    .global context_switch_to
    .type context_switch_to, %function
context_switch_to:
    ldr     r1, =interrupt_to_task
    str     r0, [r1]

    /* set from task to 0 */
    ldr     r1, =interrupt_from_task
    mov     r0, #0
    str     r0, [r1]

    /* set interrupt flag to 1 */
    ldr     r1, =switch_interrupt_flag
    mov     r0, #1
    str     r0, [r1]

    /* set kernel running to 1 */
    ldr     r1, =kernel_running
    mov     r0, #1
    str     r0, [r1]

    /* set the PendSV exception priority */
    ldr     r0, =SHPR3
    ldr     r1, =PENDSV_PRI_LOWEST
    ldr.w   r2, [r0,#0]             /* read */
    orr     r1, r1, r2              /* modify */
    str     r1, [r0]                /* write-back */

    ldr     r0, =ICSR               /* trigger the PendSV exception (causes context switch) */
    ldr     r1, =PENDSVSET_BIT
    str     r1, [r0]

    /* restore MSP */
    ldr     r0, =SCB_VTOR
    ldr     r0, [r0]
    ldr     r0, [r0]
    nop
    msr     msp, r0

    /* enable interrupts at processor level */
    cpsie   f
    cpsie   i

    /* never reach here! */
loop:
    b loop

.global HardFault_Handler
.type HardFault_Handler, %function
HardFault_Handler:
    ldr     r1, =interrupt_nest
    mov     r2, #1
    str     r2, [r1]
    /* get current context */
    mrs     r0, msp                 /* get fault context from handler. */
    tst     lr, #0x04               /* if(!EXC_RETURN[2]) */
    beq     _get_sp_done
    mrs     r0, psp                 /* get fault context from task. */
_get_sp_done:

    stmfd   r0!, {r4 - r11}         /* push r4 - r11 register */
    stmfd   r0!, {lr}               /* push exec_return register */

    tst     lr, #0x04               /* if(!EXC_RETURN[2]) */
    beq     _update_msp
    msr     psp, r0                 /* update stack pointer to psp. */
    B       _update_done
_update_msp:
    msr     msp, r0                 /* update stack pointer to MSP. */
_update_done:

    push    {lr}
    bl      cpu_hard_fault
    pop     {lr}

    orr     lr, lr, #0x04
    bx      lr
