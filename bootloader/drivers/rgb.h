/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: WS2812 RGB 状态指示灯驱动
 */

#ifndef BOOTLOADER_DRIVERS_RGB_H_
#define BOOTLOADER_DRIVERS_RGB_H_

#include <stdint.h>

/**
 * 初始化 RGB 硬件外设 (GPIOB, GPIOC, SPI2)
 */
void rgb_init(void);

/**
 * 设置全键盘单色固态显示 (R, G, B: 0~255)
 */
void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b);

#endif /* BOOTLOADER_DRIVERS_RGB_H_ */

