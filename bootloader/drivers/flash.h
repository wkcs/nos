/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: STM32F4 片上内部 Flash 驱动与内核跳转引导
 */

#ifndef BOOTLOADER_DRIVERS_FLASH_H_
#define BOOTLOADER_DRIVERS_FLASH_H_

#include <stdint.h>
#include <stdbool.h>

/* STM32F401 内部 Flash 分布 */
#define FLASH_SECTOR_0_BASE         0x08000000UL /* 16KB Bootloader */
#define FLASH_SECTOR_1_BASE         0x08004000UL /* 16KB Kernel Base */
#define FLASH_TOTAL_END             0x08040000UL /* 256KB 总上限 */

/**
 * 解锁片上 Flash 控制寄存器
 */
void flash_unlock(void);

/**
 * 加锁 Flash 并清空 CPU 缓存流水线 (Flush I-Cache & D-Cache)
 */
void flash_lock_and_flush(void);

/**
 * 擦除内部 Flash 指定扇区 (1 ~ 5)
 * 返回 0 成功，负数失败
 */
int flash_erase_sector(uint32_t sector_num);

/**
 * 按字 (32-bit Word) 编程内部 Flash
 * 返回 0 成功，负数失败
 */
int flash_program_words(uint32_t dst_addr, const uint8_t *src, uint32_t len);

/**
 * 验证当前片上内部内核固件向量表是否合法有效
 */
bool flash_is_kernel_valid(void);

/**
 * 彻底释放硬件并跳转引导至指定的应用向量基地址
 */
__attribute__((noreturn))
void flash_jump_to_app(uint32_t app_base);

#endif /* BOOTLOADER_DRIVERS_FLASH_H_ */

