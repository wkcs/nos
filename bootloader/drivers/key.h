/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 74HC595 键盘扫描与恢复按键组合检测
 */

#ifndef BOOTLOADER_DRIVERS_KEY_H_
#define BOOTLOADER_DRIVERS_KEY_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * 扫描是否有恢复按键组合按下 (Space + F 或 Space + J)
 * 返回 true 表示检测到恢复组合键，应停留在 Bootloader
 */
bool key_scan_recovery_pressed(void);

#endif /* BOOTLOADER_DRIVERS_KEY_H_ */

