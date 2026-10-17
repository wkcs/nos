/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_SYS_DIRENT_H
#define NOS_SYS_DIRENT_H
#include <sys/types.h>
typedef struct nos_directory DIR;
#ifdef NOS_LINUX_COMPAT
#define MAXNAMLEN 255
#else
#define MAXNAMLEN 63
#endif
struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[MAXNAMLEN + 1];
};
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#endif
