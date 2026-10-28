/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_DISPLAY_H__
#define __NOS_DISPLAY_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

#define BACKGROUND_LAYER 255
#define TOP_LAYER        0

enum ds_data_type {
    DS_COLOR_G = 0,
    DS_COLOR_R,
    DS_COLOR_B,
    DS_COLOR_DATA_MAX,
};

enum ds_ctrl_cmd {
    DS_CTRL_ENABLE,
    DS_CTRL_DISABLE,
    DS_CTRL_GET_ENABLE_STATUS,
    DS_CTRL_LOCK,
    DS_CTRL_UNLOCK,
    DS_CTRL_GET_DEV_INFO,
    DS_CTRL_REFRESH,
};

enum ds_data_cmd {
    DS_DATA_CMD_TRANSPARENT = 1,
};

struct display_dev_info {
    uint16_t width;
    uint16_t height;
};

struct ds_data {
    uint8_t data[DS_COLOR_DATA_MAX];
    uint8_t cmd;
};

struct ds_draw_area {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    struct ds_data *data;
};

struct ds_draw_point {
    uint16_t x;
    uint16_t y;
    struct ds_data data;
};

struct ds_draw_info {
    union {
        struct ds_draw_area area;
        struct ds_draw_point *point;
    };
    bool point_num;
    struct list_head list;
    struct mutex lock;
    uint8_t buf[];
};

struct display_server;

int ds_set_draw_point_loc(struct display_server *ds, struct ds_draw_info *info, struct ds_draw_point *point, uint16_t x, uint16_t y);
struct ds_draw_info *display_server_alloc_draw_point_info(struct display_server *ds, int point_num);
struct ds_draw_info *display_server_alloc_draw_area_info(struct display_server *ds,
                                                         uint16_t x, uint16_t y,
                                                         uint16_t width, uint16_t height);
void display_server_free_draw_info(struct display_server *ds, struct ds_draw_info *info);
static inline struct display_server *display_dev_to_server(struct device *dev)
{
    return dev->priv;
}

#endif
