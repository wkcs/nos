/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_USER_FATFS_H
#define NOS_USER_FATFS_H
#include "abi.h"
int nos_fat_mount(int format);
int nos_fat_list(const char *path, unsigned index, char *name, unsigned *mode, size_t *size);
int nos_fat_read(const char *path, void *buffer, size_t size);
int nos_fat_write(const char *path, const void *buffer, size_t size);
int nos_fat_mkdir(const char *path);
int nos_fat_unlink(const char *path);
int nos_fat_rename(const char *old, const char *next);
int nos_fat_sync(void);
#endif
