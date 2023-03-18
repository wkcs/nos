/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __BOARD_BOOT_H__
#define __BOARD_BOOT_H__

int board_write_boot_flag(u32 flag);
u32 board_read_boot_flag(void);

#endif /* __BOARD_BOOT_H__ */
