/*
 * Copyright (C) 2019 胡启航<Hu Qihang>
 *
 * Author: wkcs
 *
 * Email: hqh2030@gmail.com, huqihan@live.com
 */

.cpu cortex-m4
.syntax unified
.thumb
.text

.equ    SCB_VTOR,           0xE000ED08              /* Vector Table Offset Register */
.equ    NVIC_INT_CTRL,      0xE000ED04              /* interrupt control state register */
.equ    NVIC_SYSPRI2,       0xE000ED20              /* system priority register (2) */
.equ    NVIC_PENDSV_PRI,    0x00FF0000              /* PendSV priority value (lowest) */
.equ    NVIC_PENDSVSET,     0x10000000              /* value to trigger PendSV exception */

.global switch_interrupt_flag
.global interrupt_from_task
.global interrupt_to_task
.global kernel_running
.global interrupt_nest

/*
 * __addr_t asm_disable_irq_save();
 */
    .global asm_disable_irq_save
    .type asm_disable_irq_save, %function
asm_disable_irq_save:
    mrs     r0, primask
    cpsid   i
    bx      lr

/*
 * void asm_enable_irq_save(__addr_t level);
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

    ldr     r0, =NVIC_INT_CTRL
    ldr     r1, =NVIC_PENDSVSET
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
    cbz     r1, pendsv_exit         /* pendsv aLReady handled */

    /* clear switch_interrupt_flag to 0 */
    mov     r1, #0
    str     r1, [r0]

    ldr     r0, =interrupt_from_task
    ldr     r1, [r0]
    cbz     r1, switch_to_task      /* skip register save at the first time */

    mrs     r1, psp                 /* get from task stack pointer */
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    tst     lr, #0x10           /* if(!EXC_RETURN[4]) */
    vstmdbeq r1!, {d8 - d15}    /* push FPU register s16~s31 */
#endif
    stmfd   r1!, {r4 - r11}         /* push r4 - r11 register */
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    mov     r4, #0x00           /* flag = 0 */
    tst     lr, #0x10           /* if(!EXC_RETURN[4]) */
    moveq   r4, #0x01           /* flag = 1 */
    stmfd   r1!, {r4}           /* push flag */
#endif
    ldr     r0, [r0]
    str     r1, [r0]                /* update from task stack pointer */

switch_to_task:
    ldr     r1, =interrupt_to_task
    ldr     r1, [r1]
    ldr     r1, [r1]                /* load task stack pointer */
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    ldmfd   r1!, {r3}           /* pop flag */
#endif
    ldmfd   r1!, {r4 - r11}         /* pop r4 - r11 register */
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    cmp     r3,  #0             /* if(flag_r3 != 0) */
    vldmiane  r1!, {d8 - d15}   /* pop FPU register s16~s31 */
#endif
    msr     psp, r1                 /* update stack pointer */
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    orr     lr, lr, #0x10       /* lr |=  (1 << 4), clean FPCA. */
    cmp     r3,  #0             /* if(flag_r3 != 0) */
    bicne   lr, lr, #0x10       /* lr &= ~(1 << 4), set FPCA. */
#endif

pendsv_exit:
    /* restore interrupt */
    msr     primask, r2
    orr     lr, lr, #0x04
    bx      lr


/*
 * void context_switch_to(__addr_t to);
 * r0 --> to
 */
.global context_switch_to
.type context_switch_to, %function
context_switch_to:
    ldr     r1, =interrupt_to_task
    str     r0, [r1]

#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    /* CLEAR CONTROL.FPCA */
    mrs     r2, CONTROL         /* read */
    bic     r2, #0x04           /* modify */
    msr     CONTROL, r2         /* write-back */
#endif

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
    ldr     r0, =NVIC_SYSPRI2
    ldr     r1, =NVIC_PENDSV_PRI
    ldr.w   r2, [r0,#0]             /* read */
    orr     r1, r1, r2              /* modify */
    str     r1, [r0]                /* write-back */

    ldr     r0, =NVIC_INT_CTRL               /* trigger the PendSV exception (causes context switch) */
    ldr     r1, =NVIC_PENDSVSET
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
#if defined (__VFP_FP__) && !defined(__SOFTFP__)
    stmfd   r0!, {lr}               /* push dummy for flag */
#endif
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
