/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt Multi-Arch Board Initialization
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>

__init void board_early_init(void)
{
}

__init void board_init(void)
{
    pr_info("QEMU Virt board initializing...\n");
    hal_arch_trap_init();
    pr_info("QEMU Virt board initialized\n");
}

__init void board_late_init(void)
{
}

int board_write_boot_flag(uint32_t flag)
{
    (void)flag;
    return -ENOSYS;
}

uint32_t board_read_boot_flag(void)
{
    return 0;
}
