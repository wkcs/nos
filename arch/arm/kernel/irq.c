/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <asm/irq.h>
#include <kernel/printk.h>

/* ARM Cortex-M 系列 NVIC 寄存器基地址 */
#define NVIC_BASE           0xE000E100UL
#define NVIC_ISER           ((volatile u32 *)(NVIC_BASE + 0x000))
#define NVIC_ICER           ((volatile u32 *)(NVIC_BASE + 0x080))
#define NVIC_ISPR           ((volatile u32 *)(NVIC_BASE + 0x100))
#define NVIC_ICPR           ((volatile u32 *)(NVIC_BASE + 0x180))
#define NVIC_IPR            ((volatile u8 *)(NVIC_BASE + 0x300))

/* 系统控制块 (SCB) 寄存器 */
#define SCB_BASE            0xE000ED00UL
#define SCB_AIRCR           ((volatile u32 *)(SCB_BASE + 0x0C))
#define SCB_SHPR            ((volatile u8 *)(SCB_BASE + 0x18))

/* AIRCR 寄存器位定义 */
#define SCB_AIRCR_VECTKEY   (0x05FA << 16)
#define SCB_AIRCR_PRIGROUP  (0x07 << 8)

/**
 * 初始化架构特定的中断控制器
 */
void arch_irq_init(void)
{
    /* 设置中断优先级分组 */
    *SCB_AIRCR = SCB_AIRCR_VECTKEY | (0x03 << 8); /* 4 位抢占优先级，0 位子优先级 */
    
    pr_info("ARM NVIC initialized\n");
}

/**
 * 启用指定中断
 * @irq: 中断号
 */
void arch_irq_enable(unsigned int irq)
{
    if (irq >= 240) {
        pr_err("Invalid IRQ number: %u\n", irq);
        return;
    }
    
    NVIC_ISER[irq / 32] = (1UL << (irq % 32));
}

/**
 * 禁用指定中断
 * @irq: 中断号
 */
void arch_irq_disable(unsigned int irq)
{
    if (irq >= 240) {
        pr_err("Invalid IRQ number: %u\n", irq);
        return;
    }
    
    NVIC_ICER[irq / 32] = (1UL << (irq % 32));
}

/**
 * 设置中断优先级
 * @irq: 中断号
 * @priority: 优先级 (0 = 最高优先级)
 */
void arch_irq_set_priority(unsigned int irq, unsigned int priority)
{
    if (irq >= 240) {
        pr_err("Invalid IRQ number: %u\n", irq);
        return;
    }
    
    if (priority > 15) {
        pr_err("Invalid priority: %u\n", priority);
        return;
    }
    
    NVIC_IPR[irq] = (priority << 4);
}

/**
 * 全局禁用中断并返回之前的状态
 * @return: 之前的中断状态
 */
unsigned long arch_irq_save(void)
{
    unsigned long flags;
    
    __asm__ volatile (
        "mrs %0, primask\n"
        "cpsid i"
        : "=r" (flags)
        :
        : "memory"
    );
    
    return flags;
}

/**
 * 恢复中断状态
 * @flags: 之前保存的中断状态
 */
void arch_irq_restore(unsigned long flags)
{
    __asm__ volatile (
        "msr primask, %0"
        :
        : "r" (flags)
        : "memory"
    );
}

/**
 * 全局启用中断
 */
void arch_irq_enable_global(void)
{
    __asm__ volatile ("cpsie i" ::: "memory");
}

/**
 * 全局禁用中断
 */
void arch_irq_disable_global(void)
{
    __asm__ volatile ("cpsid i" ::: "memory");
}