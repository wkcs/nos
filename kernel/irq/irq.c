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
    return asm_disable_irq_save();
}

void enable_irq_save(addr_t level)
{
    asm_enable_irq_save(level);
}

void irq_entry()
{
    interrupt_nest++;
}

void irq_exit()
{
    if (likely(interrupt_nest > 0))
        interrupt_nest--;
}
