/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/boot.h>
#include <kernel/kernel.h>
#include <board/boot.h>

int write_boot_flag(u32 flag)
{
    return board_write_boot_flag(flag);
}

u32 read_boot_flag(void)
{
    return board_read_boot_flag();
}
