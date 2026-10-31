/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __BOARD_BOOT_H__
#define __BOARD_BOOT_H__

#include <stdint.h>
#include <stdbool.h>

/* STM32F401CCU6 Internal Flash & SRAM Geometry */
#define NK60_INT_FLASH_BASE         0x08000000UL
#define NK60_INT_FLASH_END          0x08040000UL
#define NK60_BOOTLOADER_FLASH_BASE  0x08000000UL
#define NK60_BOOTLOADER_MAX_BIN     (16UL * 1024UL)
#define NK60_KERNEL_FLASH_BASE      0x08004000UL
#define NK60_KERNEL_MAX_BIN         (240UL * 1024UL)
#define NK60_SRAM_BASE              0x20000000UL
#define NK60_SRAM_END               0x20010000UL
#define NK60_IMG_HDR_SIZE           16UL

/* NK60-V2 External W25QXX SPI NOR Flash Partition Indices */
#define NK60_PART_BOOTLOADER        0U
#define NK60_PART_KERNEL            1U
#define NK60_PART_ASSETS            2U
#define NK60_PART_COUNT             3U

/* NK60-V2 External W25QXX SPI NOR Flash Layout (4 KB sector aligned) */
#define NK60_W25Q_SECTOR_SIZE       4096UL
#define NK60_W25Q_BOOTLOADER_ADDR   0x000000UL
#define NK60_W25Q_KERNEL_ADDR       0x008000UL
#define NK60_W25Q_ASSETS_ADDR       0x048000UL
#define NK60_W25Q_ASSETS_MAX_SIZE   (736UL * 1024UL)

/* RTC Backup Register (RTC->BKP0R) Boot Commands (Zero Flash Wear) */
#define NK60_BOOT_CMD_NONE              0U
#define NK60_BOOT_CMD_UPDATE_KERNEL     1U
#define NK60_BOOT_CMD_UPDATE_BL         3U

static inline bool nk60_validate_cortexm_vectors(uint32_t msp, uint32_t reset_pc,
                                                 uint32_t flash_start,
                                                 uint32_t flash_end)
{
    if (msp < (NK60_SRAM_BASE + 0x400UL) || msp > NK60_SRAM_END ||
        (msp & 0x3UL) != 0UL) {
        return false;
    }
    if ((reset_pc & 0x1UL) == 0UL) {
        return false;
    }
    if (reset_pc < (flash_start | 1UL) || reset_pc >= flash_end) {
        return false;
    }
    return true;
}

static inline bool nk60_validate_vector_table(const uint32_t *vec,
                                              uint32_t flash_start,
                                              uint32_t flash_end)
{
    static const uint8_t req_idx[5] = { 1U, 2U, 3U, 14U, 15U };
    uint32_t i;

    if (!nk60_validate_cortexm_vectors(vec[0], vec[1], flash_start, flash_end)) {
        return false;
    }
    for (i = 0UL; i < 5UL; i++) {
        uint32_t h = vec[req_idx[i]];
        if (h != 0UL &&
            ((h & 0x1UL) == 0UL || h < (flash_start | 1UL) || h >= flash_end)) {
            return false;
        }
    }
    return true;
}

#ifndef NK60_BOOTLOADER_BUILD
struct w25q_wl;
int board_write_boot_flag(u32 flag);
u32 board_read_boot_flag(void);
struct w25q_wl *nk60_flash_get_assets_wl(void);
int nk60_assets_mount_fatfs(void);
int nk60_assets_reload(void);
#endif

#endif /* __BOARD_BOOT_H__ */
