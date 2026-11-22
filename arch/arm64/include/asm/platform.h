/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 平台中立抽象声明（具体板级地址通过 board/ 与 DTB 注入）
 */

#ifndef __ARM64_ASM_PLATFORM_H__
#define __ARM64_ASM_PLATFORM_H__

#include <stdint.h>

uint64_t arm64_get_boot_dtb_phys(void);
uint8_t arm64_get_boot_el(void);

#endif /* __ARM64_ASM_PLATFORM_H__ */
