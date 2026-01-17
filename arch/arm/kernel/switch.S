/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

.syntax unified
.thumb

.global __arch_switch_to

/**
 * ARM Cortex-M 系列上下文切换汇编实现
 * @r0: prev context (struct cpu_context *)
 * @r1: next context (struct cpu_context *)
 */
.section .text.__arch_switch_to
.type __arch_switch_to, %function
__arch_switch_to:
    /* 保存当前任务的上下文到 prev */
    stmia   r0!, {r4-r11}       /* 保存 r4-r11 */
    str     sp, [r0, #0]        /* 保存 sp */
    str     lr, [r0, #4]        /* 保存 lr */
    
    /* 从 next 恢复新任务的上下文 */
    ldmia   r1!, {r4-r11}       /* 恢复 r4-r11 */
    ldr     sp, [r1, #0]        /* 恢复 sp */
    ldr     lr, [r1, #4]        /* 恢复 lr */
    
    /* 返回到新任务 */
    bx      lr

.size __arch_switch_to, .-__arch_switch_to