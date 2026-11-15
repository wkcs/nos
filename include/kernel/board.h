/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __KERNEL_BOARD_H__
#define __KERNEL_BOARD_H__

#include <stdint.h>
#include <stddef.h>

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

/* Boards without persistent boot flags return -ENOSYS from write. */
int board_write_boot_flag(uint32_t flag);
uint32_t board_read_boot_flag(void);

/**
 * 读取芯片硬件唯一标识符 (UID)
 *
 * @param uid 存储 12 字节 UID 的缓冲区
 * @return 0 成功，负数表示错误码
 */
int board_get_chip_uid(uint8_t uid[12]);

/**
 * 读取芯片硬件 UID 的十六进制字符串表示
 *
 * @param buf 存储十六进制字符串的缓冲区 (至少 25 字节)
 * @param max_len 缓冲区最大长度
 * @return 0 成功，负数表示错误码
 */
int board_get_chip_uid_hex(char *buf, size_t max_len);

#endif /* __KERNEL_BOARD_H__ */
