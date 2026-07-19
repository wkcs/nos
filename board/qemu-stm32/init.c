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
#include "stm32f10x.h"
#include "board.h"

__init void board_init(void)
{
    pr_info("QEMU STM32 board initializing...\n");

    board_mm_init();

    arch_irq_init();
    arch_mmu_init();

    pr_info("QEMU STM32 board initialized\n");
}
