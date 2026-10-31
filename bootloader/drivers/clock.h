/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 系统时钟、延时与 RTC 备份域管理
 */

#ifndef BOOTLOADER_DRIVERS_CLOCK_H_
#define BOOTLOADER_DRIVERS_CLOCK_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * 初始化系统时钟为 84MHz，同时生成 48MHz USB 时钟
 * 并使能电源控制 (PWR) 与 RTC 备份域写访问权限
 */
void clock_init_84mhz(void);

/**
 * 软件微秒级延时
 */
void delay_us(uint32_t us);

/**
 * 软件毫秒级延时
 */
void delay_ms(uint32_t ms);

/**
 * 基础循环周期延时
 */
void delay_cycles(volatile uint32_t cycles);

/**
 * 读取 RTC 备份寄存器 0 (RTC_BKP0R)
 */
uint32_t backup_read_flag(void);

/**
 * 写入 RTC 备份寄存器 0 (RTC_BKP0R)
 */
void backup_write_flag(uint32_t val);

/**
 * 清除 RTC 备份寄存器 0 (RTC_BKP0R)
 */
void backup_clear_flag(void);

#endif /* BOOTLOADER_DRIVERS_CLOCK_H_ */

