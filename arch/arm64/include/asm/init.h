/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 初始化钩子接口
 */

#ifndef __ARM64_ASM_INIT_H__
#define __ARM64_ASM_INIT_H__

#include <stdint.h>
#include <kernel/kernel.h>

void arch_early_init(void);
void arch_init(void);
void nos_arm64_boot(uint64_t dtb_pa, uint64_t boot_el);

#endif /* __ARM64_ASM_INIT_H__ */
