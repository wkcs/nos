/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include "board.h"
#include "stm32f4xx_rtc.h"

#define BOOT_SAVE_ADDR RTC_BKP_DR0

int board_write_boot_flag(u32 flag)
{
    RTC_WriteBackupRegister(BOOT_SAVE_ADDR, flag);

    return 0;
}

u32 board_read_boot_flag(void)
{
    u32 flag;

    flag = RTC_ReadBackupRegister(BOOT_SAVE_ADDR);

    return flag;
}
