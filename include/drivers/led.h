/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_LED_H__
#define __NOS_LED_H__

enum color_type {
    COLOR_G = 0,
    COLOR_R,
    COLOR_B,
    COLOR_MAX,
};

enum led_ctrl_cmd {
    LED_CTRL_ENABLE,
    LED_CTRL_DISABLE,
    LED_CTRL_GET_ENABLE_STATUS,
    LED_CTRL_REFRESH,
};

#endif
