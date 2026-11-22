/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 架构中断管理与动态 IRQ 注册接口
 */

#ifndef __ASM_ARM64_IRQ_H__
#define __ASM_ARM64_IRQ_H__

#include <stdint.h>
#include <kernel/types.h>

typedef void (*nos_irq_handler_t)(uint32_t intid, void *ctx);

void arch_irq_init(void);
void arch_irq_enable(unsigned int irq);
void arch_irq_disable(unsigned int irq);
void arch_irq_set_priority(unsigned int irq, unsigned int priority);
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);
void arch_irq_enable_global(void);
void arch_irq_disable_global(void);

/* 动态内核中断注册与分发接口 */
int nos_irq_register(uint32_t intid, nos_irq_handler_t handler, void *ctx);
int nos_irq_unregister(uint32_t intid);
void arm64_irq_dispatch(void);

/* 内核 IRQ 临界区保存与恢复原语（供 kernel/irq/irq.c 调用） */
addr_t asm_disable_irq_save(void);
void asm_enable_irq_save(addr_t level);

#endif /* __ASM_ARM64_IRQ_H__ */
