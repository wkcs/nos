/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2007        */
/*-----------------------------------------------------------------------*/
/* This is a block device front end that attaches FatFs to the common API. */
/*-----------------------------------------------------------------------*/
#include <drivers/block.h>
#include "diskio.h"
#ifdef CONFIG_SHELL_PIPE_TEST
#include <kernel/sleep.h>
#include <kernel/task.h>
#endif

#ifdef CONFIG_BLOCK_CACHE
#include <drivers/block_cache.h>

#ifndef CONFIG_BLOCK_CACHE_SECTORS
#define CONFIG_BLOCK_CACHE_SECTORS 8
#endif

static struct block_cache g_fatfs_cache;
static bool g_fatfs_cache_inited;

static void fatfs_cache_cleanup(void)
{
    if (g_fatfs_cache_inited) {
        (void)block_cache_sync(&g_fatfs_cache);
        block_cache_deinit(&g_fatfs_cache);
        g_fatfs_cache_inited = false;
    }
}

static void fatfs_cache_setup(const struct device *dev)
{
    uint32_t sector_size = 512;
    uint32_t sector_count = 0;

    if (dev == NULL || !device_is_ready(dev)) {
        return;
    }

    if (g_fatfs_cache_inited) {
        if (g_fatfs_cache.raw_bdev == dev) {
            return;
        }
        fatfs_cache_cleanup();
    }

    if (block_get_info(dev, &sector_size, &sector_count) != 0 || sector_size == 0) {
        sector_size = 512;
    }

    if (block_cache_init(&g_fatfs_cache, dev, CONFIG_BLOCK_CACHE_SECTORS, sector_size) == 0) {
        g_fatfs_cache_inited = true;
    }
}
#endif

#define FATFS_DRIVE             0
#define FATFS_DEFAULT_DEVICE    "ramdisk0"

static const struct device *fatfs_block_dev;

void fatfs_set_block_device(const struct device *dev)
{
#ifdef CONFIG_BLOCK_CACHE
    if (fatfs_block_dev != dev) {
        fatfs_cache_cleanup();
    }
#endif
    fatfs_block_dev = dev;
#ifdef CONFIG_BLOCK_CACHE
    if (dev != NULL) {
        fatfs_cache_setup(dev);
    }
#endif
}

static const struct device *fatfs_get_block_device(void)
{
    if (fatfs_block_dev == NULL) {
        fatfs_block_dev = device_get_binding(FATFS_DEFAULT_DEVICE);
    }
#ifdef CONFIG_BLOCK_CACHE
    if (fatfs_block_dev != NULL && !g_fatfs_cache_inited) {
        fatfs_cache_setup(fatfs_block_dev);
    }
#endif
    return fatfs_block_dev;
}

DSTATUS disk_status(
    BYTE pdrv
)
{
    const struct device *dev;

    if (pdrv != FATFS_DRIVE) {
        return STA_NOINIT;
    }

    dev = fatfs_get_block_device();
    return device_is_ready(dev) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(
    BYTE pdrv
)
{
    return disk_status(pdrv);
}

DRESULT disk_read(
    BYTE pdrv,
    BYTE *buff,
    DWORD sector,
    UINT count
)
{
    const struct device *dev;

    if (pdrv != FATFS_DRIVE || buff == NULL || count == 0) {
        return RES_PARERR;
    }

    dev = fatfs_get_block_device();
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

#ifdef CONFIG_SHELL_PIPE_TEST
    if (task_current() != NULL) msleep(1);
#endif

#ifdef CONFIG_BLOCK_CACHE
    if (g_fatfs_cache_inited) {
        return block_cache_read(&g_fatfs_cache, sector, buff, count) == 0 ? RES_OK : RES_ERROR;
    }
#endif

    return block_read(dev, sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}

#if _USE_WRITE
DRESULT disk_write(
    BYTE pdrv,
    const BYTE *buff,
    DWORD sector,
    UINT count
)
{
    const struct device *dev;

    if (pdrv != FATFS_DRIVE || buff == NULL || count == 0) {
        return RES_PARERR;
    }

    dev = fatfs_get_block_device();
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

#ifdef CONFIG_SHELL_PIPE_TEST
    if (task_current() != NULL) msleep(1);
#endif

#ifdef CONFIG_BLOCK_CACHE
    if (g_fatfs_cache_inited) {
        return block_cache_write(&g_fatfs_cache, sector, buff, count) == 0 ? RES_OK : RES_ERROR;
    }
#endif

    return block_write(dev, sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}
#endif

#if _USE_IOCTL
DRESULT disk_ioctl(
    BYTE pdrv,
    BYTE cmd,
    void *buff
)
{
    const struct device *dev;
    uint32_t sector_size;
    uint32_t sector_count;

    if (pdrv != FATFS_DRIVE) {
        return RES_PARERR;
    }

    dev = fatfs_get_block_device();
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
#ifdef CONFIG_BLOCK_CACHE
        if (g_fatfs_cache_inited) {
            return block_cache_sync(&g_fatfs_cache) == 0 ? RES_OK : RES_ERROR;
        }
#endif
        return RES_OK;
    case GET_SECTOR_COUNT:
    case GET_SECTOR_SIZE:
    case GET_BLOCK_SIZE:
        if (buff == NULL ||
            block_get_info(dev, &sector_size, &sector_count) != 0) {
            return RES_ERROR;
        }
        if (cmd == GET_SECTOR_COUNT) {
            *(DWORD *)buff = sector_count;
        } else if (cmd == GET_SECTOR_SIZE) {
            *(WORD *)buff = (WORD)sector_size;
        } else {
            *(DWORD *)buff = 1;
        }
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
#endif

__weak DWORD get_fattime(void) {
	/* 返回当前时间戳 */
	return	  ((DWORD)(2015 - 1980) << 25)	/* Year 2015 */
			| ((DWORD)1 << 21)				/* Month 1 */
			| ((DWORD)1 << 16)				/* Mday 1 */
			| ((DWORD)0 << 11)				/* Hour 0 */
			| ((DWORD)0 << 5)				  /* Min 0 */
			| ((DWORD)0 >> 1);				/* Sec 0 */
}
