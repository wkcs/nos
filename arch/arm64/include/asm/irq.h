/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_ARM64_IRQ_H__
#define __ASM_ARM64_IRQ_H__

#include <kernel/types.h>

/* ARM64 架构中断管理接口（契约对齐 arch/arm；GIC 细节后续填充） */

/* GIC-v3 中断类型 (reserved for future board bring-up) */
#define GIC_SGI_BASE        0
#define GIC_PPI_BASE        16
#define GIC_SPI_BASE        32

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

/* CPU 汇编实现见 arch/arm64 （stub 阶段未实现） */
addr_t asm_disable_irq_save(void);
void asm_enable_irq_save(addr_t level);

#endif /* __ASM_ARM64_IRQ_H__ */
