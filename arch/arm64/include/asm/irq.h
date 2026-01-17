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

/* ARM64 架构中断管理接口 */

/* GIC-v3 中断类型 */
#define GIC_SGI_BASE        0       /* 软件生成中断 (0-15) */
#define GIC_PPI_BASE        16      /* 私有外设中断 (16-31) */
#define GIC_SPI_BASE        32      /* 共享外设中断 (32-1019) */

/**
 * 初始化 GIC-v3 中断控制器
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
 * 设置中断亲和性 (SPI only)
 * @irq: 中断号
 * @affinity: CPU 亲和性掩码
 */
void arch_irq_set_affinity(unsigned int irq, u64 affinity);

/**
 * 发送核间中断 (SGI)
 * @sgi: SGI 中断号 (0-15)
 * @target_list: 目标 CPU 列表
 */
void arch_send_sgi(unsigned int sgi, u64 target_list);

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

#endif /* __ASM_ARM64_IRQ_H__ */