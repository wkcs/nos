/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/boot.h>
#include <kernel/kernel.h>
#include <kernel/board.h>

int write_boot_flag(u32 flag)
{
    /* 自动附加高 16 位 Magic，确保重启后 Bootloader 可识别目标模式 */
    u32 raw = (get_boot_flag_magic(flag) == BOOT_FLAG_MAGIC) ?
              flag : get_boot_flag_data(flag);
    return board_write_boot_flag(raw);
}

u32 read_boot_flag(void)
{
    u32 raw = board_read_boot_flag();
    if (get_boot_flag_magic(raw) != BOOT_FLAG_MAGIC) {
        return BOOT_NORMAL;
    }
    return get_boot_flag(raw);
}
