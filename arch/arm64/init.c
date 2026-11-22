/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <asm/init.h>
#include <asm/cpufeature.h>
#include <kernel/mm.h>
#include <asm/mmu.h>

__init void arch_early_init(void)
{
}

__init void arch_init(void)
{
    arm64_cpufeature_dump();
    arch_mmu_init();
}
