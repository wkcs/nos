/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include "stm32f10x.h"
#include "board.h"

__init void board_init(void)
{
    board_mm_init();
}
