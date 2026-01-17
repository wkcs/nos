/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_ARM_IRQ_H__
#define __ASM_ARM_IRQ_H__

#include <kernel/types.h>

/* ARM 架构中断管理接口 */

/**
 * 初始化架构特定的中断控制器
 */
void arch_irq_init(void);

/**
 * 启用指定中断
 * @irq: 中断号
 */
void arch_irq_enable(unsigned int irq);

/**
 * 禁用指定中断
 * @irq: 中断号
 */
void arch_irq_disable(unsigned int irq);

/**
 * 设置中断优先级
 * @irq: 中断号
 * @priority: 优先级 (0 = 最高优先级)
 */
void arch_irq_set_priority(unsigned int irq, unsigned int priority);

/**
 * 全局禁用中断并返回之前的状态
 * @return: 之前的中断状态
 */
unsigned long arch_irq_save(void);

/**
 * 恢复中断状态
 * @flags: 之前保存的中断状态
 */
void arch_irq_restore(unsigned long flags);

/**
 * 全局启用中断
 */
void arch_irq_enable_global(void);

/**
 * 全局禁用中断
 */
void arch_irq_disable_global(void);

#endif /* __ASM_ARM_IRQ_H__ */