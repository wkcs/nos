/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * RISC-V (RV32 / RV64) Architecture Early Initialization
 */

#include <asm/init.h>
#include <nos/hal_arch.h>

__init void arch_early_init(void)
{
}

__init void arch_init(void)
{
    hal_arch_trap_init();
}
