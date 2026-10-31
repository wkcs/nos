/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_W25Q_FLASH_H__
#define __NOS_DRIVERS_W25Q_FLASH_H__

#include <drivers/block.h>
#include <drivers/w25q_spi.h>
#include <kernel/mutex.h>

#define W25Q_FLASH_CTRL_ERASE_SECTOR 0x01

#define W25Q_WL_PHYS_SECTOR_SIZE     4096U
#define W25Q_WL_LOGICAL_SECTOR_SIZE  512U
#define W25Q_WL_SUBSECTORS_PER_BLOCK (W25Q_WL_PHYS_SECTOR_SIZE / W25Q_WL_LOGICAL_SECTOR_SIZE)
#define W25Q_WL_META_SECTORS         2U
#define W25Q_WL_MAX_BLOCKS           256U
#define W25Q_WL_STATIC_THRESHOLD     4U
#define W25Q_WL_META_MAGIC           0x57323557UL /* "W25W" */

struct w25q_flash {
    const struct w25q_spi *spi;
    uint32_t capacity;
    struct mutex lock;
    uint8_t page_buf[256];
};

struct w25q_wl {
    struct w25q_flash *flash;
    uint32_t base_addr;
    uint32_t region_size;
    uint16_t total_phys_sectors;
    uint16_t data_phys_sectors;
    uint16_t spare_sectors;
    uint16_t logical_blocks;
    uint32_t meta_seq;
    uint8_t  active_meta_idx;
    uint16_t next_meta_off;
    uint32_t total_erases;
    uint32_t static_migrations;
    uint16_t l2p[W25Q_WL_MAX_BLOCKS];
    uint16_t erase_counts[W25Q_WL_MAX_BLOCKS];
    uint8_t  used_bitmap[W25Q_WL_MAX_BLOCKS / 8U];
};

extern const struct block_driver_api w25q_wl_block_driver_api;

int w25q_flash_init(struct w25q_flash *flash, const struct w25q_spi *spi,
                    uint32_t capacity);
int w25q_flash_read(struct w25q_flash *flash, uint32_t address,
                    void *buffer, size_t length);
int w25q_flash_write(struct w25q_flash *flash, uint32_t address,
                     const void *buffer, size_t length);
int w25q_flash_erase(struct w25q_flash *flash, uint32_t address,
                     size_t length);

int w25q_wl_init(struct w25q_wl *wl, struct w25q_flash *flash,
                 uint32_t base_addr, uint32_t region_size);
int w25q_wl_reset(struct w25q_wl *wl);
int w25q_wl_read(struct w25q_wl *wl, uint32_t lba, void *buf, uint32_t count);
int w25q_wl_write(struct w25q_wl *wl, uint32_t lba, const void *buf,
                  uint32_t count);
int w25q_wl_get_info(const struct w25q_wl *wl, uint32_t *sector_size,
                     uint32_t *sector_count);

#endif /* __NOS_DRIVERS_W25Q_FLASH_H__ */
