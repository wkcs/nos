/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt Multi-Arch Board Initialization
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>

__init void board_early_init(void)
{
}

__init void board_init(void)
{
    pr_info("QEMU Virt board initializing...\r\n");
    hal_arch_trap_init();
    pr_info("QEMU Virt board initialized\r\n");
}

__init void board_late_init(void)
{
}
