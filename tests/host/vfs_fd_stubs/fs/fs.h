#ifndef TEST_VFS_FD_FS_H
#define TEST_VFS_FD_FS_H
#include <sys/types.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
typedef long long loff_t;
int nos_close(int fd);
int nos_dup(int oldfd);
int nos_dup2(int oldfd, int newfd);
ssize_t nos_read(int fd, void *buf, size_t count);
ssize_t nos_write(int fd, const void *buf, size_t count);
loff_t nos_lseek(int fd, loff_t offset, int whence);
int nos_sync(void);
int nos_fsync(int fd);
int nos_fdatasync(int fd);
#endif
