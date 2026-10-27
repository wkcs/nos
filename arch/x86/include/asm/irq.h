/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __ASM_X86_IRQ_H__
#define __ASM_X86_IRQ_H__

#include <kernel/types.h>

void arch_irq_init(void);
void arch_irq_enable(unsigned int irq);
void arch_irq_disable(unsigned int irq);
void arch_irq_set_priority(unsigned int irq, unsigned int priority);
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);
void arch_irq_enable_global(void);
void arch_irq_disable_global(void);
addr_t asm_disable_irq_save(void);
void asm_enable_irq_save(addr_t level);

#endif /* __ASM_X86_IRQ_H__ */
