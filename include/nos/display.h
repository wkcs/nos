/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_DISPLAY_H__
#define __NOS_DISPLAY_H__

#include <kernel/device.h>
#include <kernel/list.h>
#include <kernel/mutex.h>

#define DS_LAYER_OPAQUE 255U

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
    int point_num;
    int16_t z_order;
    uint8_t opacity;
    struct list_head list;
    struct mutex lock;
    uint8_t buf[];
};

struct display_server;

struct ds_frame {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
};

struct ds_render_layer {
    int16_t z_order;
    uint8_t opacity;
    /* Called under the composition lock. Render without blocking or reentering the server. */
    void (*render)(struct ds_frame *frame, uint8_t opacity, void *ctx);
    void *ctx;
};

struct ds_layer_source {
    /* Return a frame snapshot sorted by z-order, valid until the next prepare. */
    const struct ds_render_layer *(*prepare)(void *ctx, size_t *count);
    void *ctx;
};

/*
 * Register before scheduling starts. The source, layers and contexts must live
 * for the lifetime of the output. Higher z-order values are composited later.
 * At equal z-order, buffered drawings cover render callbacks, and older buffered
 * drawings cover newer ones to preserve existing callers' stacking order.
 */
int display_server_register_layers(const char *output, const struct ds_layer_source *source);
void display_server_set_draw_layer(struct display_server *ds, struct ds_draw_info *info,
                                   int16_t z_order, uint8_t opacity);

static inline void display_frame_blend_pixel(struct ds_frame *frame, uint16_t x, uint16_t y,
                                             const uint8_t pixel[DS_COLOR_DATA_MAX],
                                             uint8_t opacity)
{
    size_t index;
    unsigned int channel;

    if (x >= frame->width || y >= frame->height || opacity == 0U) {
        return;
    }
    index = ((size_t)y * frame->width + x) * DS_COLOR_DATA_MAX;
    for (channel = 0U; channel < DS_COLOR_DATA_MAX; channel++) {
        unsigned int below = frame->pixels[index + channel];
        frame->pixels[index + channel] =
            (uint8_t)((pixel[channel] * opacity +
                       below * (DS_LAYER_OPAQUE - opacity) + 127U) / DS_LAYER_OPAQUE);
    }
}

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
