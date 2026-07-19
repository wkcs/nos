/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <asm/irq.h>
#include <asm/mmu.h>

/* ZJ-V3 板级配置 */

/**
 * 板级初始化
 */
void board_init(void)
{
    pr_info("ZJ-V3 board initializing...\n");
    
    /* 初始化架构特定的中断控制器 */
    arch_irq_init();
    
    /* 初始化 MMU (如果支持) */
    arch_mmu_init();
    
    /* 板级特定的初始化 */
    /* TODO: 添加板级外设初始化 */
    
    pr_info("ZJ-V3 board initialized\n");
}

/**
 * 板级早期初始化 (在架构初始化之前)
 */
void board_early_init(void)
{
    /* 早期板级初始化，如时钟配置等 */
}

/**
 * 板级后期初始化 (在内核初始化之后)
 */
void board_late_init(void)
{
    /* 后期板级初始化，如外设驱动加载等 */
}