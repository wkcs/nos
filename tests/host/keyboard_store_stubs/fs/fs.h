/** Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#ifndef TEST_STORE_FS_H
#define TEST_STORE_FS_H
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
typedef off_t loff_t;
int nos_open(const char *, int, ...);
int nos_close(int);
ssize_t nos_read(int, void *, size_t);
ssize_t nos_write(int, const void *, size_t);
loff_t nos_lseek(int, loff_t, int);
int nos_mkdir(const char *, mode_t);
int nos_rename(const char *, const char *);
int nos_fsync(int);
int nos_unlink(const char *);
int nos_rmdir(const char *);
#endif
