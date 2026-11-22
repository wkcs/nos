/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 早期 Fixmap 与设备树只读访问辅助
 */

#include <stddef.h>
#include <stdint.h>
#include <asm/mmu.h>

void *arm64_early_phys_map(uintptr_t paddr)
{
    return (void *)arm64_phys_to_virt(paddr);
}
