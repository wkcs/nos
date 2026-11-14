/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片内 FMC LittleFS 存储分区块设备驱动
 */
#include <kernel/kernel.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <drivers/block.h>
#include <soc/nuc472_soc.h>
#include <string.h>

#ifndef CONFIG_NUC472_LFS_BASE
#define CONFIG_NUC472_LFS_BASE 0x00060000UL
#endif

#define FMC_NUC472_SECTOR_SIZE  512U
#define FMC_NUC472_SECTOR_COUNT 256U
#define FMC_NUC472_PAGE_SECTORS 4U    /* 2048 / 512 = 4 */

#ifndef NUC472_HOST_TEST
#define NUC472_FLASH_ADDR_TO_PTR(addr) ((const void *)(uintptr_t)(addr))
#else
extern const void *nuc472_test_flash_ptr(uint32_t addr);
#define NUC472_FLASH_ADDR_TO_PTR(addr) nuc472_test_flash_ptr(addr)
#endif

static struct mutex g_fmc_nuc472_lock;

static int fmc_nuc472_read(const struct device *dev, uint32_t lba, void *buf,
                           uint32_t count)
{
    uint32_t offset;
    const uint8_t *src;

    if (dev == NULL || buf == NULL) {
        return -EINVAL;
    }
    if (count == 0U) {
        return 0;
    }
    if (lba >= FMC_NUC472_SECTOR_COUNT || count > (FMC_NUC472_SECTOR_COUNT - lba)) {
        return -EINVAL;
    }

    offset = lba * FMC_NUC472_SECTOR_SIZE;
    src = (const uint8_t *)NUC472_FLASH_ADDR_TO_PTR(CONFIG_NUC472_LFS_BASE + offset);
    if (src == NULL) {
        return -EIO;
    }

    mutex_lock(&g_fmc_nuc472_lock);
    memcpy(buf, src, count * FMC_NUC472_SECTOR_SIZE);
    mutex_unlock(&g_fmc_nuc472_lock);

    return 0;
}

static int fmc_nuc472_write(const struct device *dev, uint32_t lba, const void *buf,
                            uint32_t count)
{
    uint32_t total_bytes;
    uint32_t base_addr;
    const uint8_t *src_bytes;

    if (dev == NULL || buf == NULL) {
        return -EINVAL;
    }
    if (count == 0U) {
        return 0;
    }
    if (lba >= FMC_NUC472_SECTOR_COUNT || count > (FMC_NUC472_SECTOR_COUNT - lba)) {
        return -EINVAL;
    }

    total_bytes = count * FMC_NUC472_SECTOR_SIZE;
    base_addr = CONFIG_NUC472_LFS_BASE + (lba * FMC_NUC472_SECTOR_SIZE);
    src_bytes = (const uint8_t *)buf;

    mutex_lock(&g_fmc_nuc472_lock);

    for (uint32_t off = 0U; off < total_bytes; off += 4U) {
        uint32_t curr_addr = base_addr + off;
        const uint32_t *flash_ptr = (const uint32_t *)NUC472_FLASH_ADDR_TO_PTR(curr_addr);
        uint32_t old_val;
        uint32_t new_val;
        int ret;

        if (flash_ptr == NULL) {
            mutex_unlock(&g_fmc_nuc472_lock);
            return -EIO;
        }

        old_val = *flash_ptr;
        memcpy(&new_val, &src_bytes[off], sizeof(uint32_t));

        if (new_val == old_val) {
            continue;
        }

        /* 物理 Flash 只支持 1 改 0，若新值需要将已有 0 改为 1 则强制要求先擦除 */
        if ((new_val & ~old_val) != 0U) {
            mutex_unlock(&g_fmc_nuc472_lock);
            return -EIO;
        }

        ret = nuc472_fmc_write_u32(curr_addr, new_val);
        if (ret != 0) {
            mutex_unlock(&g_fmc_nuc472_lock);
            return ret;
        }
    }

    mutex_unlock(&g_fmc_nuc472_lock);
    return 0;
}

static int fmc_nuc472_erase(const struct device *dev, uint32_t lba, uint32_t count)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    if (count == 0U) {
        return 0;
    }
    if (lba >= FMC_NUC472_SECTOR_COUNT || count > (FMC_NUC472_SECTOR_COUNT - lba)) {
        return -EINVAL;
    }
    /* 擦除粒度必须按 2048 字节 (4 扇区) 物理页对齐 */
    if ((lba % FMC_NUC472_PAGE_SECTORS) != 0U || (count % FMC_NUC472_PAGE_SECTORS) != 0U) {
        return -EINVAL;
    }

    mutex_lock(&g_fmc_nuc472_lock);

    for (uint32_t s = lba; s < (lba + count); s += FMC_NUC472_PAGE_SECTORS) {
        uint32_t page_addr = CONFIG_NUC472_LFS_BASE + (s * FMC_NUC472_SECTOR_SIZE);
        int ret = nuc472_fmc_erase_page(page_addr);
        if (ret != 0) {
            mutex_unlock(&g_fmc_nuc472_lock);
            return ret;
        }
    }

    mutex_unlock(&g_fmc_nuc472_lock);
    return 0;
}

static int fmc_nuc472_get_info(const struct device *dev, uint32_t *sector_size,
                               uint32_t *sector_count)
{
    (void)dev;
    if (sector_size == NULL || sector_count == NULL) {
        return -EINVAL;
    }

    *sector_size = FMC_NUC472_SECTOR_SIZE;
    *sector_count = FMC_NUC472_SECTOR_COUNT;
    return 0;
}

static const struct block_driver_api fmc_nuc472_block_api = {
    .read = fmc_nuc472_read,
    .write = fmc_nuc472_write,
    .erase = fmc_nuc472_erase,
    .get_info = fmc_nuc472_get_info,
};

static int fmc_nuc472_init(struct device *dev)
{
    (void)dev;
    mutex_init(&g_fmc_nuc472_lock);
    return 0;
}

DEVICE_DEFINE(nuc472_lfs, "nuc472-lfs", fmc_nuc472_init, NULL, NULL,
              &fmc_nuc472_block_api, POST_KERNEL);
