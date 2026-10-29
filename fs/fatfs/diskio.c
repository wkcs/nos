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

#define FATFS_DRIVE             0
#define FATFS_DEFAULT_DEVICE    "ramdisk0"

static const struct device *fatfs_block_dev;

void fatfs_set_block_device(const struct device *dev)
{
    fatfs_block_dev = dev;
}

static const struct device *fatfs_get_block_device(void)
{
    if (fatfs_block_dev == NULL) {
        fatfs_block_dev = device_get_binding(FATFS_DEFAULT_DEVICE);
    }

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

