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
#include "ffconf.h"

#ifndef CONFIG_BLOCK_CACHE_SECTORS
#define CONFIG_BLOCK_CACHE_SECTORS 8
#endif

#ifndef _VOLUMES
#define _VOLUMES 2
#endif

static struct block_cache g_fatfs_cache[_VOLUMES];
static bool g_fatfs_cache_inited[_VOLUMES];

static void fatfs_cache_cleanup(BYTE pdrv)
{
    if (pdrv < _VOLUMES && g_fatfs_cache_inited[pdrv]) {
        (void)block_cache_sync(&g_fatfs_cache[pdrv]);
        block_cache_deinit(&g_fatfs_cache[pdrv]);
        g_fatfs_cache_inited[pdrv] = false;
    }
}

static void fatfs_cache_setup(BYTE pdrv, const struct device *dev)
{
    uint32_t sector_size = 512;
    uint32_t sector_count = 0;

    if (pdrv >= _VOLUMES || dev == NULL || !device_is_ready(dev)) {
        return;
    }

    if (g_fatfs_cache_inited[pdrv]) {
        if (g_fatfs_cache[pdrv].raw_bdev == dev) {
            return;
        }
        fatfs_cache_cleanup(pdrv);
    }

    if (block_get_info(dev, &sector_size, &sector_count) != 0 || sector_size == 0) {
        sector_size = 512;
    }

    if (block_cache_init(&g_fatfs_cache[pdrv], dev, CONFIG_BLOCK_CACHE_SECTORS, sector_size) == 0) {
        g_fatfs_cache_inited[pdrv] = true;
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
        fatfs_cache_cleanup(FATFS_DRIVE);
    }
#endif
    fatfs_block_dev = dev;
#ifdef CONFIG_BLOCK_CACHE
    if (dev != NULL) {
        fatfs_cache_setup(FATFS_DRIVE, dev);
    }
#endif
}

static const struct device *fatfs_get_block_device(BYTE pdrv)
{
    (void)pdrv;
    if (fatfs_block_dev == NULL) {
        fatfs_block_dev = device_get_binding(FATFS_DEFAULT_DEVICE);
    }
#ifdef CONFIG_BLOCK_CACHE
    if (pdrv < _VOLUMES && fatfs_block_dev != NULL && !g_fatfs_cache_inited[pdrv]) {
        fatfs_cache_setup(pdrv, fatfs_block_dev);
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

    dev = fatfs_get_block_device(pdrv);
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

    dev = fatfs_get_block_device(pdrv);
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

#ifdef CONFIG_SHELL_PIPE_TEST
    if (task_current() != NULL) msleep(1);
#endif

#ifdef CONFIG_BLOCK_CACHE
    if (pdrv < _VOLUMES && g_fatfs_cache_inited[pdrv]) {
        return block_cache_read(&g_fatfs_cache[pdrv], sector, buff, count) == 0 ? RES_OK : RES_ERROR;
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

    dev = fatfs_get_block_device(pdrv);
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

#ifdef CONFIG_SHELL_PIPE_TEST
    if (task_current() != NULL) msleep(1);
#endif

#ifdef CONFIG_BLOCK_CACHE
    if (pdrv < _VOLUMES && g_fatfs_cache_inited[pdrv]) {
        return block_cache_write(&g_fatfs_cache[pdrv], sector, buff, count) == 0 ? RES_OK : RES_ERROR;
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

    dev = fatfs_get_block_device(pdrv);
    if (!device_is_ready(dev)) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
#ifdef CONFIG_BLOCK_CACHE
        if (pdrv < _VOLUMES && g_fatfs_cache_inited[pdrv]) {
            return block_cache_sync(&g_fatfs_cache[pdrv]) == 0 ? RES_OK : RES_ERROR;
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
