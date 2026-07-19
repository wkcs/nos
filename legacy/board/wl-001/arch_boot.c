/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include "board.h"
#include "stm32f10x_bkp.h"

#define BOOT_SAVE_ADDR1 BKP_DR1
#define BOOT_SAVE_ADDR2 BKP_DR2

int board_write_boot_flag(u32 flag)
{
    BKP_WriteBackupRegister(BOOT_SAVE_ADDR1, flag & 0xffff);
    BKP_WriteBackupRegister(BOOT_SAVE_ADDR2, (flag >> 16) & 0xffff);

    return 0;
}

u32 board_read_boot_flag(void)
{
    u32 flag;

    flag = BKP_ReadBackupRegister(BOOT_SAVE_ADDR1) & 0xffff;
    flag = (flag << 16) | (BKP_ReadBackupRegister(BOOT_SAVE_ADDR2) & 0xffff);

    return flag;
}
