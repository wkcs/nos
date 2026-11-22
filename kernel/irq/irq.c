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
#include <nos/hal_arch.h>
#include <asm/irq.h>

u32 irq_disable_level;
volatile u32 interrupt_nest;
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
static volatile u32 g_interrupt_nest[CONFIG_NR_CPUS];

static inline uint32_t irq_cpu_idx(void)
{
    uint32_t cpu = hal_cpu_id();
    return (cpu < (uint32_t)CONFIG_NR_CPUS) ? cpu : 0U;
}
#endif

addr_t disable_irq_save()
{
    return asm_disable_irq_save();
}

void enable_irq_save(addr_t level)
{
    asm_enable_irq_save(level);
}

void irq_entry(void)
{
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    g_interrupt_nest[irq_cpu_idx()]++;
#else
    interrupt_nest++;
#endif
}

void irq_exit(void)
{
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    uint32_t cpu = irq_cpu_idx();
    if (likely(g_interrupt_nest[cpu] > 0U)) {
        g_interrupt_nest[cpu]--;
    }
#else
    if (likely(interrupt_nest > 0)) {
        interrupt_nest--;
    }
#endif
}

uint32_t irq_nest_count(void)
{
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    return g_interrupt_nest[irq_cpu_idx()];
#else
    return interrupt_nest;
#endif
}
