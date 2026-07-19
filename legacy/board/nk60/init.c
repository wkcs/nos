/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include "stm32f4xx.h"
#include "board.h"

__init void board_init(void)
{
    gpio_config(&cc_gpio[0]);
    gpio_config(&cc_gpio[1]);
    board_mm_init();
}
