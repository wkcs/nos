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

#define BOOT_FLAG_MAGIC 0x20300000
#define BOOT_FLAG_MAGIC_MASK 0xffff0000
#define BOOT_FLAG_MASK 0x0000ffff

#define get_boot_flag(data) (BOOT_FLAG_MASK & (data))
#define get_boot_flag_magic(data) (BOOT_FLAG_MAGIC_MASK & (data))
#define get_boot_flag_data(flag) ((BOOT_FLAG_MASK & (flag)) | BOOT_FLAG_MAGIC)

enum boot_flag {
    BOOT_NORMAL = 0,
    BOOT_BOOTLOADER,
};

int write_boot_flag(u32 flag);
u32 read_boot_flag(void);

#endif /* __NOS_BOOT_H__ */
