/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_DISPLAY_H__
#define __NOS_DISPLAY_H__

#define BACKGROUND_LAYER 255
#define TOP_LAYER        0

enum ds_data_type {
    DS_COLOR_G = 0,
    DS_COLOR_R,
    DS_COLOR_B,
    DS_COLOR_LAYER,
    DS_COLOR_DATA_MAX,
};

enum ds_ctrl_cmd {
    DS_CTRL_ENABLE,
    DS_CTRL_DISABLE,
    DS_CTRL_GET_ENABLE_STATUS,
    DS_CTRL_LOCK,
    DS_CTRL_UNLOCK,
    DS_CTRL_CLEAR_LAYER,
};

#endif
