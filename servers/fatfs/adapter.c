/* Copyright (C) 2026 NOS contributors */
#include "adapter.h"
#include "../drivers/devices.h"
#ifndef NOS_ARM_M
#include "ff.h"
#include "diskio.h"
static FATFS filesystem;
static int fat_errno(FRESULT result)
{
    switch (result) {
    case FR_OK: return 0;
    case FR_NO_FILE: case FR_NO_PATH: return -NOS_ENOENT;
    case FR_EXIST: return -NOS_EEXIST;
    case FR_DENIED: case FR_WRITE_PROTECTED: return -NOS_EACCES;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: return -NOS_EINVAL;
    case FR_NOT_ENOUGH_CORE: return -NOS_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return -NOS_EMFILE;
    case FR_NO_FILESYSTEM: return -NOS_ENOTSUP;
    default: return -NOS_EIO;
    }
}
DSTATUS disk_initialize(BYTE drive) { return drive || nos_block_init() ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return drive ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE drive, BYTE *buffer, DWORD sector, UINT count)
{
    if (drive || !count) return RES_PARERR;
    for (UINT i = 0; i < count; i++) if (nos_block_read(sector + i, buffer + i * 512)) return RES_ERROR;
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buffer, DWORD sector, UINT count)
{
    if (drive || !count) return RES_PARERR;
    for (UINT i = 0; i < count; i++) if (nos_block_write(sector + i, buffer + i * 512)) return RES_ERROR;
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive) return RES_PARERR;
    if (command == CTRL_SYNC) return nos_block_flush() ? RES_ERROR : RES_OK;
    if (command == GET_SECTOR_COUNT) { uint64_t sectors = nos_block_capacity(); if (sectors > 0xffffffffu) return RES_PARERR; *(DWORD *)buffer = (DWORD)sectors; return RES_OK; }
    if (command == GET_BLOCK_SIZE) { *(DWORD *)buffer = 1; return RES_OK; }
    return RES_PARERR;
}
int nos_fat_mount(int format)
{
    int rc = nos_block_init(); if (rc) return rc;
    FRESULT result = f_mount(&filesystem, "0:", 1);
    if (format && result == FR_NO_FILESYSTEM) { result = f_mkfs("0:", 1, 0); if (!result) result = f_mount(&filesystem, "0:", 1); }
    return fat_errno(result);
}
int nos_fat_list(const char *path, unsigned index, char *name, unsigned *mode, size_t *size)
{
    DIR dir; FILINFO info; FRESULT result = f_opendir(&dir, path); if (result) return fat_errno(result);
    for (unsigned i = 0; i <= index; i++) { result = f_readdir(&dir, &info); if (result || !info.fname[0]) break; }
    f_closedir(&dir); if (result) return fat_errno(result);
    if (!info.fname[0]) return 0;
    u_memcpy(name, info.fname, u_strlen(info.fname) + 1); *mode = info.fattrib & AM_DIR ? 0040000u : 0100000u; *size = info.fsize; return 1;
}
int nos_fat_read(const char *path, void *buffer, size_t size)
{
    FIL file; UINT done = 0; FRESULT result = f_open(&file, path, FA_READ); if (result) return fat_errno(result);
    result = f_read(&file, buffer, (UINT)size, &done); FRESULT close = f_close(&file);
    return result ? fat_errno(result) : close ? fat_errno(close) : (int)done;
}
int nos_fat_write(const char *path, const void *buffer, size_t size)
{
    FIL file; UINT done = 0; FRESULT result = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS); if (result) return fat_errno(result);
    result = f_write(&file, buffer, (UINT)size, &done); FRESULT close = f_close(&file);
    return result ? fat_errno(result) : close ? fat_errno(close) : done != size ? -NOS_ENOSPC : 0;
}
int nos_fat_mkdir(const char *path) { return fat_errno(f_mkdir(path)); }
int nos_fat_unlink(const char *path) { return fat_errno(f_unlink(path)); }
int nos_fat_rename(const char *old, const char *next) { return fat_errno(f_rename(old, next)); }
int nos_fat_sync(void) { return nos_block_flush(); }
#else
int nos_fat_mount(int format) { (void)format; return -NOS_ENOTSUP; }
int nos_fat_list(const char *path, unsigned i, char *n, unsigned *m, size_t *s)
{ (void)path; (void)i; (void)n; (void)m; (void)s; return -NOS_ENOTSUP; }
int nos_fat_read(const char *p, void *b, size_t n) { (void)p; (void)b; (void)n; return -NOS_ENOTSUP; }
int nos_fat_write(const char *p, const void *b, size_t n) { (void)p; (void)b; (void)n; return -NOS_ENOTSUP; }
int nos_fat_mkdir(const char *p) { (void)p; return -NOS_ENOTSUP; }
int nos_fat_unlink(const char *p) { (void)p; return -NOS_ENOTSUP; }
int nos_fat_rename(const char *p, const char *n) { (void)p; (void)n; return -NOS_ENOTSUP; }
int nos_fat_sync(void) { return -NOS_ENOTSUP; }
#endif
