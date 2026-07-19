/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#include "stm32f4xx.h"
#include "board.h"

/**
 * 板级早期初始化 (在架构初始化之前)
 */
__init void board_early_init(void)
{
    /* 时钟等早期工作仍由 SystemInit / Reset 路径完成 */
}

__init void board_init(void)
{
    pr_info("NK60 board initializing...\r\n");

    gpio_config(&cc_gpio[0]);
    gpio_config(&cc_gpio[1]);
    board_mm_init();

    /* TODO: move arch_irq_init/arch_mmu_init to arch_init */
    arch_irq_init();
    arch_mmu_init();

    pr_info("NK60 board initialized\r\n");
}

/**
 * 板级后期初始化 (在内核初始化之后)
 */
__init void board_late_init(void)
{
}
