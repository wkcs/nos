/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_BOOT_H__
#define __NOS_BOOT_H__

#include <kernel/kernel.h>

/* 高 16 位 Magic 用于区分冷上电复位值 (0x00000000) 与显式重启标志 */
#define BOOT_FLAG_MAGIC         0x20300000UL
#define BOOT_FLAG_MAGIC_MASK    0xFFFF0000UL
#define BOOT_FLAG_MASK          0x0000FFFFUL

#define get_boot_flag(data)       ((u32)(BOOT_FLAG_MASK & (u32)(data)))
#define get_boot_flag_magic(data) ((u32)(BOOT_FLAG_MAGIC_MASK & (u32)(data)))
#define get_boot_flag_data(flag)  ((u32)(((u32)(flag) & BOOT_FLAG_MASK) | BOOT_FLAG_MAGIC))

enum boot_flag {
    BOOT_NORMAL     = 0,
    BOOT_BOOTLOADER = 1,
    BOOT_RECOVERY   = 2,
};

int write_boot_flag(u32 flag);
u32 read_boot_flag(void);

#endif /* __NOS_BOOT_H__ */
