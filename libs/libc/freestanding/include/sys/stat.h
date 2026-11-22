/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <sys/stat.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_SYS_STAT_H__
#define __NOS_FREESTANDING_SYS_STAT_H__

#include <sys/types.h>
#include <fs/fs.h>

struct stat {
    dev_t      st_dev;
    uint64_t   st_ino;
    mode_t     st_mode;
    uint32_t   st_nlink;
    uint32_t   st_uid;
    uint32_t   st_gid;
    dev_t      st_rdev;
    off_t      st_size;
    int64_t    st_atime;
    int64_t    st_mtime;
    int64_t    st_ctime;
    int64_t    st_blksize;
    int64_t    st_blocks;
};

#endif /* __NOS_FREESTANDING_SYS_STAT_H__ */
