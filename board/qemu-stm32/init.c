/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#include <soc/stm32f4xx.h>
#include "board.h"

/**
 * 板级早期初始化 (在架构初始化之前)
 */
__init void board_early_init(void)
{
    /* 早期板级初始化，如时钟配置等 */
}

__init void board_init(void)
{
    pr_info("QEMU STM32 board initializing...\n");

    board_mm_init();

    /* TODO: move arch_irq_init/arch_mmu_init to arch_init */
    arch_irq_init();
    arch_mmu_init();

    pr_info("QEMU STM32 board initialized\n");
}

/**
 * 板级后期初始化 (在内核初始化之后)
 */
__init void board_late_init(void)
{
    /* 后期板级初始化，如外设驱动加载等 */
}
