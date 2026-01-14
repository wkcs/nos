/**
 * @file qemu_sd.c
 * @brief SD Card Driver for QEMU using Semihosting
 */

#include "qemu_sd.h"
#include <string.h>
#include <kernel/printk.h>
#include "../../fs/fatfs/diskio.h" /* For DSTATUS/DRESULT definitions */

/* Semihosting Operations */
#define SYS_OPEN   0x01
#define SYS_CLOSE  0x02
#define SYS_WRITEC 0x03
#define SYS_WRITE0 0x04
#define SYS_WRITE  0x05
#define SYS_READ   0x06
#define SYS_READC  0x07
#define SYS_ISERROR 0x08
#define SYS_ISTTY  0x09
#define SYS_SEEK   0x0A
#define SYS_FLEN   0x0C
#define SYS_TMPNAM 0x0D
#define SYS_REMOVE 0x0E
#define SYS_RENAME 0x0F
#define SYS_CLOCK  0x10
#define SYS_TIME   0x11
#define SYS_ERRNO  0x13

static int semi_call(int action, void *arg) {
    int result;
    asm volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "bkpt 0xAB\n\t"
        "mov %0, r0\n\t"
        : "=r" (result)
        : "r" (action), "r" (arg)
        : "r0", "r1", "memory"
    );
    return result;
}

static long fd = -1;
static uint32_t file_len = 0;

QEMU_SD_CardInfo SDCardInfo;

uint8_t QEMU_SD_Init (uint8_t pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    
    if (fd != -1) return 0; // Already open

    /* Open disk.img */
    const char * name = "disk.img";
    uint32_t args[3];
    args[0] = (uint32_t)name;
    args[1] = 3; // "r+b"
    args[2] = strlen(name);

    pr_info("QEMU_SD: Try open %s\n", name);
    fd = semi_call(SYS_OPEN, args);
    pr_info("QEMU_SD: fd = %d\n", fd);

    if (fd == -1) return STA_NOINIT;

    /* Get file length */
    uint32_t flen_args[1] = { (uint32_t)fd };
    int len = semi_call(SYS_FLEN, flen_args);
    pr_info("QEMU_SD: len = %d\n", len);

    if (len == -1) {
        // Failed to get length?
        file_len = 64 * 1024 * 1024; // Default 64MB
    } else {
        file_len = len;
    }

    SDCardInfo.CardBlockSize = 512;
    SDCardInfo.CardCapacity = file_len;

    return 0;
}

uint8_t QEMU_SD_Status (uint8_t pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (fd == -1) return STA_NOINIT;
    return 0;
}

uint8_t QEMU_SD_Read (uint8_t pdrv, uint8_t *buff, uint32_t sector, uint32_t count) {
    if (pdrv != 0 || fd == -1) return RES_PARERR;
    if (count == 0) return RES_PARERR;

    /* Seek */
    uint32_t offset = sector * 512;
    uint32_t seek_args[2] = { (uint32_t)fd, offset };
    if (semi_call(SYS_SEEK, seek_args) != 0) return RES_ERROR;

    /* Read */
    uint32_t read_args[3] = { (uint32_t)fd, (uint32_t)buff, count * 512 };
    int res = semi_call(SYS_READ, read_args);
    
    if (res == 0) return RES_OK;
    if (res == (count * 512)) return RES_PARERR; // Nothing read
    return RES_ERROR; // Partial read?
}

uint8_t QEMU_SD_Write (uint8_t pdrv, const uint8_t *buff, uint32_t sector, uint32_t count) {
    if (pdrv != 0 || fd == -1) return RES_PARERR;
    if (count == 0) return RES_PARERR;

    /* Seek */
    uint32_t offset = sector * 512;
    uint32_t seek_args[2] = { (uint32_t)fd, offset };
    if (semi_call(SYS_SEEK, seek_args) != 0) return RES_ERROR;

    /* Write */
    uint32_t write_args[3] = { (uint32_t)fd, (uint32_t)buff, count * 512 };
    int res = semi_call(SYS_WRITE, write_args);

    if (res == 0) return RES_OK;
    return RES_ERROR;
}

uint8_t QEMU_SD_Ioctl (uint8_t pdrv, uint8_t cmd, void *buff) {
    if (pdrv != 0 || fd == -1) return RES_PARERR;

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD*)buff = file_len / 512;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 1;
            return RES_OK;
    }
    return RES_PARERR;
}
