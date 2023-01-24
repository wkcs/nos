/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <asm/irq.h>

u32 irq_disable_level;
volatile u32 interrupt_nest;

addr_t disable_irq_save()
{
    BUG_ON(irq_disable_level >= U32_MAX);
    irq_disable_level++;
    return asm_disable_irq_save();
}

void enable_irq_save(addr_t level)
{
    if (irq_disable_level > 0) {
        irq_disable_level--;
    }
    if (irq_disable_level == 0) {
        asm_enable_irq_save(level);
    }
}
