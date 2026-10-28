/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_FS_H__
#define __NOS_FS_H__

#include <stddef.h>
#include <kernel/types.h>

#ifndef _SSIZE_T_DECLARED
typedef int ssize_t;
#define _SSIZE_T_DECLARED
#endif

#ifndef O_ACCMODE
#define O_ACCMODE 00000003
#endif
#ifndef O_RDONLY
#define O_RDONLY 00000000
#endif
#ifndef O_WRONLY
#define O_WRONLY 00000001
#endif
#ifndef O_RDWR
#define O_RDWR 00000002
#endif
#ifndef O_CREAT
#define O_CREAT 00000100  /* not fcntl */
#endif
#ifndef O_EXCL
#define O_EXCL 00000200   /* not fcntl */
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 00000400 /* not fcntl */
#endif
#ifndef O_TRUNC
#define O_TRUNC 00001000  /* not fcntl */
#endif
#ifndef O_APPEND
#define O_APPEND 00002000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 00004000
#endif
#ifndef O_DSYNC
#define O_DSYNC 00010000  /* used to be O_SYNC, see below */
#endif
#ifndef FASYNC
#define FASYNC 00020000   /* fcntl, for BSD compatibility */
#endif
#ifndef O_DIRECT
#define O_DIRECT 00040000 /* direct disk access hint */
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 00100000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 00200000 /* must be a directory */
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 00400000  /* don't follow links */
#endif
#ifndef O_NOATIME
#define O_NOATIME 01000000
#endif

#ifndef SEEK_SET
#define SEEK_SET 0 /* seek relative to beginning of file */
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1 /* seek relative to current file position */
#endif
#ifndef SEEK_END
#define SEEK_END 2 /* seek relative to end of file */
#endif

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

#ifndef _MODE_T_DECLARED
typedef unsigned int mode_t;
#define _MODE_T_DECLARED
#endif
typedef long long
    loff_t; /* Changed to long long to match vfs.h and standard practice */
#ifndef _DEV_T_DECLARED
typedef unsigned int dev_t; /* Changed to unsigned int */
#define _DEV_T_DECLARED
#endif

/* File types */
#ifndef S_IFMT
#define S_IFMT 0170000
#endif
#ifndef S_IFREG
#define S_IFREG 0100000
#endif
#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif
#ifndef S_IFCHR
#define S_IFCHR 0020000
#endif
#ifndef S_IFBLK
#define S_IFBLK 0060000
#endif
#ifndef S_IFIFO
#define S_IFIFO 0010000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

#ifndef S_IRWXU
#define S_IRWXU 00700
#endif
#ifndef S_IRUSR
#define S_IRUSR 00400
#endif
#ifndef S_IWUSR
#define S_IWUSR 00200
#endif
#ifndef S_IXUSR
#define S_IXUSR 00100
#endif

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
int nos_unlink(const char *pathname);
int nos_rename(const char *oldpath, const char *newpath);
int nos_truncate(const char *pathname, loff_t length);
int vfs_bind_mount(struct dentry *target, struct dentry *mnt_root); // Add declaration

/* Filesystem Initialization */
int init_ramfs(void);
int init_fatfs(void);
int init_procfs(void);
int init_sysfs(void);

#endif /* __NOS_FS_H__ */
