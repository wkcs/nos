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
/*
 * addr_t asm_disable_irq_save();
 */
    .global asm_disable_irq_save
    .type asm_disable_irq_save, %function
asm_disable_irq_save:
    mrs     r0, PRIMASK
    cpsid   I
    bx      lr

/*
 * void asm_enable_irq_save(addr_t level);
 */
    .global asm_enable_irq_save
    .type asm_enable_irq_save, %function
asm_enable_irq_save:
    msr     PRIMASK, r0
    bx      lr
