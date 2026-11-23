/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[DISPLAY_SERVER]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <nos/completion.h>
#include <nos/timeout.h>
#include <kernel/list.h>

#include <drivers/led.h>
#include <nos/display.h>

struct display_server {
    struct task_struct *task;
    struct device *led_dev;
    struct device ds_dev;
    struct display_dev_info dev_info;
    struct nos_completion comp;
    struct mutex lock;
    struct list_head draw_list;

    bool led_enable;

    struct ds_frame frame;
    const struct ds_layer_source *source;
};

static const struct ds_layer_source *g_layer_source;

int display_server_register_layers(const char *output, const struct ds_layer_source *source)
{
    if (output == NULL || source == NULL || source->prepare == NULL) {
        return -EINVAL;
    }
    if (strcmp(output, CONFIG_LED_DEV) != 0) {
        return -ENODEV;
    }
    if (g_layer_source != NULL) {
        return -EBUSY;
    }
    g_layer_source = source;
    return 0;
}

static void display_server_insert_draw_layer(struct display_server *ds, struct ds_draw_info *info)
{
    struct ds_draw_info *layer;

    list_for_each_entry(layer, &ds->draw_list, list) {
        if (layer->z_order >= info->z_order) {
            list_add_tail(&info->list, &layer->list);
            return;
        }
    }
    list_add_tail(&info->list, &ds->draw_list);
}

void display_server_set_draw_layer(struct display_server *ds, struct ds_draw_info *info,
                                   int16_t z_order, uint8_t opacity)
{
    mutex_lock(&ds->lock);
    if (info->z_order != z_order) {
        list_del(&info->list);
        info->z_order = z_order;
        display_server_insert_draw_layer(ds, info);
    }
    info->opacity = opacity;
    mutex_unlock(&ds->lock);
}

int ds_set_draw_point_loc(struct display_server *ds, struct ds_draw_info *info, struct ds_draw_point *point, uint16_t x, uint16_t y)
{
    if (info == NULL) {
        pr_err("draw info is NULL\n");
        return -EINVAL;
    }
    if (point == NULL) {
        pr_err("point is NULL\n");
        return -EINVAL;
    }

    if (x >= ds->dev_info.width) {
        return -EINVAL;
    }
    if (y >= ds->dev_info.height) {
        return -EINVAL;
    }

    mutex_lock(&info->lock);
    point->x = x;
    point->y = y;
    mutex_unlock(&info->lock);

    return 0;
}

struct ds_draw_info *display_server_alloc_draw_point_info(struct display_server *ds, int point_num)
{
    struct ds_draw_info *info;
    int i;

    if (point_num <= 0 ||
        (size_t)point_num > (SIZE_MAX - sizeof(*info)) / sizeof(struct ds_draw_point)) {
        return NULL;
    }
    info = kmalloc(sizeof(*info) + sizeof(struct ds_draw_point) * (size_t)point_num, GFP_KERNEL);
    if (info == NULL) {
        pr_err("alloc draw info failed\n");
        return NULL;
    }

    info->point_num = point_num;
    info->point = (struct ds_draw_point *)info->buf;
    for (i = 0; i < point_num; i++) {
        memset(&info->point[i], 0, sizeof(struct ds_draw_point));
    }
    info->z_order = 0;
    info->opacity = DS_LAYER_OPAQUE;
    mutex_init(&info->lock);
    mutex_lock(&ds->lock);
    display_server_insert_draw_layer(ds, info);
    mutex_unlock(&ds->lock);

    return info;
}

struct ds_draw_info *display_server_alloc_draw_area_info(struct display_server *ds,
                                                         uint16_t x, uint16_t y,
                                                         uint16_t width, uint16_t height)
{
    struct ds_draw_info *info;

    if (x + width > ds->dev_info.width) {
        pr_err("x is out of range, x=%u, x_max=%u\n",
               x + width, ds->dev_info.width);
        return NULL;
    }
    if (y + height > ds->dev_info.height) {
        pr_err("y is out of range, y=%u, y_max=%u\n",
               y + height, ds->dev_info.height);
        return NULL;
    }

    if (width == 0U || height == 0U ||
        (size_t)width * height > (SIZE_MAX - sizeof(*info)) / sizeof(struct ds_data)) {
        return NULL;
    }
    info = kmalloc(sizeof(*info) + sizeof(struct ds_data) * width * height, GFP_KERNEL);
    if (info == NULL) {
        pr_err("alloc draw info failed\n");
        return NULL;
    }

    info->point_num = 0;
    info->area.x = x;
    info->area.y = y;
    info->area.width = width;
    info->area.height = height;
    info->area.data = (struct ds_data *)info->buf;
    memset(info->buf, 0, sizeof(struct ds_data) * width * height);
    info->z_order = 0;
    info->opacity = DS_LAYER_OPAQUE;
    mutex_init(&info->lock);
    mutex_lock(&ds->lock);
    display_server_insert_draw_layer(ds, info);
    mutex_unlock(&ds->lock);

    return info;
}

void display_server_free_draw_info(struct display_server *ds, struct ds_draw_info *info)
{
    mutex_lock(&ds->lock);
    list_del(&info->list);
    mutex_unlock(&ds->lock);
    kfree(info);
}

static int display_server_control(struct device *dev, int cmd, void *args)
{
    struct display_server *ds = dev->priv;

    switch (cmd) {
    case DS_CTRL_ENABLE:
        ds->led_dev->ops.control(ds->led_dev, LED_CTRL_ENABLE, NULL);
        ds->led_enable = true;
        nos_complete(&ds->comp);
        break;
    case DS_CTRL_DISABLE:
        ds->led_enable = false;
        ds->led_dev->ops.control(ds->led_dev, LED_CTRL_DISABLE, NULL);
        break;
    case DS_CTRL_GET_ENABLE_STATUS:
        return ds->led_enable;
    case DS_CTRL_LOCK:
        mutex_lock(&ds->lock);
        break;
    case DS_CTRL_UNLOCK:
        mutex_unlock(&ds->lock);
        break;
    case DS_CTRL_GET_DEV_INFO:
        memcpy(args, &ds->dev_info, sizeof(struct display_dev_info));
        break;
    default:
        pr_err("unknown cmd: %d\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void display_server_merge_draw_layer(struct ds_frame *frame, const struct ds_draw_info *layer)
{
    const struct ds_draw_area *area = &layer->area;
    int i;
    uint16_t y;

    if (layer->point_num != 0) {
        for (i = 0; i < layer->point_num; i++) {
            const struct ds_draw_point *point = &layer->point[i];

            if (point->data.cmd != DS_DATA_CMD_TRANSPARENT) {
                display_frame_blend_pixel(frame, point->x, point->y, point->data.data,
                                          layer->opacity);
            }
        }
        return;
    }
    for (y = 0U; y < area->height; y++) {
        uint16_t x;

        for (x = 0U; x < area->width; x++) {
            const struct ds_data *pixel = &area->data[(size_t)y * area->width + x];

            if (pixel->cmd != DS_DATA_CMD_TRANSPARENT) {
                display_frame_blend_pixel(frame, area->x + x, area->y + y, pixel->data,
                                          layer->opacity);
            }
        }
    }
}

static void display_server_merge_buf(struct display_server *ds)
{
    const struct ds_layer_source *source = ds->source;
    const struct ds_render_layer *layers = NULL;
    size_t count = 0U;
    size_t i = 0U;
    struct ds_draw_info *draw;

    if (source != NULL) {
        layers = source->prepare(source->ctx, &count);
        if (layers == NULL) {
            count = 0U;
        }
    }
    memset(ds->frame.pixels, 0, (size_t)ds->frame.width * ds->frame.height * DS_COLOR_DATA_MAX);
    mutex_lock(&ds->lock);
    list_for_each_entry(draw, &ds->draw_list, list) {
        while (i < count && layers[i].z_order <= draw->z_order) {
            layers[i].render(&ds->frame, layers[i].opacity, layers[i].ctx);
            i++;
        }
        mutex_lock(&draw->lock);
        display_server_merge_draw_layer(&ds->frame, draw);
        mutex_unlock(&draw->lock);
    }
    while (i < count) {
        layers[i].render(&ds->frame, layers[i].opacity, layers[i].ctx);
        i++;
    }
    mutex_unlock(&ds->lock);
}

static void display_server_task_entry(void* parameter)
{
    struct display_server *ds = parameter;
    int i;

    ds->led_dev = NULL;
    for (i = 0; i < 100; i ++) {
        ds->led_dev = device_find_by_name(CONFIG_LED_DEV);
        if (ds->led_dev == NULL) {
            i++;
            pr_err("%s device not found, retry=%d\n", CONFIG_LED_DEV, i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\n", CONFIG_LED_DEV);
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\n", CONFIG_LED_DEV);
        return;
    }

    ds->led_dev->ops.control(ds->led_dev, DS_CTRL_GET_DEV_INFO, &ds->dev_info);
    ds->frame.width = ds->dev_info.width;
    ds->frame.height = ds->dev_info.height;
    ds->frame.pixels = kmalloc((size_t)ds->frame.width * ds->frame.height * DS_COLOR_DATA_MAX,
                               GFP_KERNEL);
    if (ds->frame.pixels == NULL) {
        pr_err("alloc display buf error\n");
        return;
    }
    pr_info("display device:[%s] %u*%u\n", CONFIG_LED_DEV, ds->dev_info.width, ds->dev_info.height);

    /*
     * 注册了图层源（如键盘灯效）时，由图层源所有者通过 DS_CTRL_ENABLE 决定何时点亮，
     * 上电默认保持 LED 断电，避免配置加载前出现任何灯光。
     */
    ds->led_enable = ds->source == NULL;
    if (ds->led_enable) {
        ds->led_dev->ops.control(ds->led_dev, LED_CTRL_ENABLE, NULL);
    }

    device_register(&ds->ds_dev);

    while (true) {
        if (ds->led_enable) {
            display_server_merge_buf(ds);
            ds->led_dev->ops.write(ds->led_dev, 0, ds->frame.pixels,
                (size_t)ds->frame.width * ds->frame.height * DS_COLOR_DATA_MAX);
            ds->led_dev->ops.control(ds->led_dev, LED_CTRL_REFRESH, NULL);
            msleep(10);
        } else {
            nos_completion_wait(&ds->comp, NOS_WAIT_FOREVER);
        }
    }
}

static int display_server_init(void)
{
    struct display_server *ds;

    ds = kmalloc(sizeof(struct display_server), GFP_KERNEL);
    if (ds == NULL) {
        pr_err("alloc display_server buf error\n");
        return -ENOMEM;
    }

    ds->source = g_layer_source;
    nos_completion_init(&ds->comp);
    mutex_init(&ds->lock);
    INIT_LIST_HEAD(&ds->draw_list);

    device_init(&ds->ds_dev);
    ds->ds_dev.name = "display-server";
    ds->ds_dev.ops.control = display_server_control;
    ds->ds_dev.priv = ds;

    ds->task = task_create("display_server", display_server_task_entry, ds, 2, 1024, 10, NULL);
    if (ds->task == NULL) {
        pr_fatal("creat display_server task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ds->task);

    return 0;
}
task_init(display_server_init);
