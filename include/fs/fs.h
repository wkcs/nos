/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_FS_H__
#define __NOS_FS_H__

#include <kernel/types.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int size_t;
typedef int ssize_t;
#endif

#define O_ACCMODE 00000003
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR 00000002
#define O_CREAT 00000100  /* not fcntl */
#define O_EXCL 00000200   /* not fcntl */
#define O_NOCTTY 00000400 /* not fcntl */
#define O_TRUNC 00001000  /* not fcntl */
#define O_APPEND 00002000
#define O_NONBLOCK 00004000
#define O_DSYNC 00010000  /* used to be O_SYNC, see below */
#define FASYNC 00020000   /* fcntl, for BSD compatibility */
#define O_DIRECT 00040000 /* direct disk access hint */
#define O_LARGEFILE 00100000
#define O_DIRECTORY 00200000 /* must be a directory */
#define O_NOFOLLOW 00400000  /* don't follow links */
#define O_NOATIME 01000000

#define SEEK_SET 0 /* seek relative to beginning of file */
#define SEEK_CUR 1 /* seek relative to current file position */
#define SEEK_END 2 /* seek relative to end of file */

/*
 * File types
 */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

typedef unsigned int mode_t;
typedef long long
    loff_t; /* Changed to long long to match vfs.h and standard practice */
typedef unsigned int dev_t; /* Changed to unsigned int */

/* File types */
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

struct qstr {
  unsigned int hash;
  unsigned int len;
  const unsigned char *name;
};

struct path {
  struct vfsmount *mnt;
  struct dentry *dentry;
};

/* VFS Public API */
int nos_open(const char *path, int flags, ...);
int nos_close(int fd);
ssize_t nos_read(int fd, void *buf, size_t count);
ssize_t nos_write(int fd, const void *buf, size_t count);
loff_t nos_lseek(int fd, loff_t offset, int whence);
int nos_mkdir(const char *pathname, mode_t mode);
int nos_rmdir(const char *pathname);
int vfs_bind_mount(struct dentry *target, struct dentry *mnt_root); // Add declaration

/* Filesystem Initialization */
int init_ramfs(void);
int init_fatfs(void);
int init_procfs(void);
int init_sysfs(void);

#endif /* __NOS_FS_H__ */
