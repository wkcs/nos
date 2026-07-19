/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
/* QEMU STM32 板级配置 */

/**
 * 板级早期初始化 (在架构初始化之前)
 */
void board_early_init(void)
{
    /* 早期板级初始化，如时钟配置等 */
}

/**
 * 板级后期初始化 (在内核初始化之后)
 */
void board_late_init(void)
{
    /* 后期板级初始化，如外设驱动加载等 */
}