#ifndef TEST_VFS_FD_FS_H
#define TEST_VFS_FD_FS_H
#include <sys/types.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
typedef long long loff_t;
ssize_t nos_read(int fd, void *buf, size_t count);
ssize_t nos_write(int fd, const void *buf, size_t count);
loff_t nos_lseek(int fd, loff_t offset, int whence);
#endif
