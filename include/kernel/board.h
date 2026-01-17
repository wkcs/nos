/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __KERNEL_BOARD_H__
#define __KERNEL_BOARD_H__

/* 板级支持接口 */

/**
 * 板级早期初始化 (在架构初始化之前)
 */
void board_early_init(void);

/**
 * 板级初始化
 */
void board_init(void);

/**
 * 板级后期初始化 (在内核初始化之后)
 */
void board_late_init(void);

#endif /* __KERNEL_BOARD_H__ */