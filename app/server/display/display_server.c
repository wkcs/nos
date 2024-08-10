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
#include <kernel/sem.h>
#include <kernel/list.h>

#include <display/led.h>
#include <display/display.h>

struct display_server {
    struct task_struct *task;
    struct device *led_dev;
    struct device ds_dev;
    struct display_dev_info dev_info;
    sem_t sem;
    struct mutex lock;
    struct list_head draw_list;

    bool led_enable;

    uint8_t *buf;
};

int ds_set_draw_point_loc(struct display_server *ds, struct ds_draw_info *info, struct ds_draw_point *point, uint16_t x, uint16_t y)
{
    if (info == NULL) {
        pr_err("draw info is NULL\r\n");
        return -EINVAL;
    }
    if (point == NULL) {
        pr_err("point is NULL\r\n");
        return -EINVAL;
    }

    if (x >= ds->dev_info.width) {
        pr_err("x is out of range, x_max=%u\r\n", ds->dev_info.width);
    }
    if (y >= ds->dev_info.height) {
        pr_err("y is out of range, y_max=%u\r\n", ds->dev_info.height);
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

    info = kmalloc(sizeof(struct ds_draw_info) + sizeof(struct ds_draw_point) * point_num, GFP_KERNEL);
    if (info == NULL) {
        pr_err("alloc draw info failed\r\n");
        return NULL;
    }

    info->point_num = point_num;
    info->point = (struct ds_draw_point *)info->buf;
    for (i = 0; i < point_num; i++) {
        memset(&info->point[i], 0, sizeof(struct ds_draw_point));
    }
    mutex_init(&info->lock);
    mutex_lock(&ds->lock);
    list_add(&info->list, &ds->draw_list);
    mutex_unlock(&ds->lock);

    return info;
}

struct ds_draw_info *display_server_alloc_draw_area_info(struct display_server *ds,
                                                         uint16_t x, uint16_t y,
                                                         uint16_t width, uint16_t height)
{
    struct ds_draw_info *info;

    if (x + width > ds->dev_info.width) {
        pr_err("x is out of range, x=%u, x_max=%u\r\n",
               x + width, ds->dev_info.width);
        return NULL;
    }
    if (y + height > ds->dev_info.height) {
        pr_err("y is out of range, y=%u, y_max=%u\r\n",
               y + height, ds->dev_info.height);
        return NULL;
    }

    info = kmalloc(sizeof(struct ds_draw_info) + sizeof(struct ds_data) * width * height, GFP_KERNEL);
    if (info == NULL) {
        pr_err("alloc draw info failed\r\n");
        return NULL;
    }

    info->point_num = 0;
    info->area.x = x;
    info->area.y = y;
    info->area.width = width;
    info->area.height = height;
    info->area.data = (struct ds_data *)info->buf;
    memset(info->buf, 0, sizeof(struct ds_data) * width * height);
    mutex_init(&info->lock);
    mutex_lock(&ds->lock);
    list_add(&info->list, &ds->draw_list);
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
        sem_send_one(&ds->sem);
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
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void display_server_merge_point(struct display_server *ds, struct ds_draw_point *point)
{
    int index;

    if (point->data.cmd == DS_DATA_CMD_TRANSPARENT)
        return;

    index = (point->y * ds->dev_info.width + point->x) * DS_COLOR_DATA_MAX;
    memcpy(ds->buf + index, point->data.data, DS_COLOR_DATA_MAX);
}

static void display_server_merge_area(struct display_server *ds, struct ds_draw_area *area)
{
    int x, y;
    int index;
    int data_index;

    for (y = area->y; y < area->y + area->height; y++) {
        for (x = area->x; x < area->x + area->width; x++) {
            data_index = (y - area->y) * area->width + (x - area->x);
            if (area->data[data_index].cmd == DS_DATA_CMD_TRANSPARENT)
                continue;
            index = (y * ds->dev_info.width + x) * DS_COLOR_DATA_MAX;
            memcpy(ds->buf + index, area->data[data_index].data, DS_COLOR_DATA_MAX);
        }
    }
}

static void display_server_merge_buf(struct display_server *ds)
{
    struct ds_draw_info *di;
    int i;

    mutex_lock(&ds->lock);
    memset(ds->buf, 0, ds->dev_info.width * ds->dev_info.height * DS_COLOR_DATA_MAX);
    list_for_each_entry(di, &ds->draw_list, list) {
        mutex_lock(&di->lock);
        if (di->point_num) {
            for (i = 0; i < di->point_num; i++) {
                display_server_merge_point(ds, &di->point[i]);
            }
        } else {
            display_server_merge_area(ds, &di->area);
        }
        mutex_unlock(&di->lock);
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
            pr_err("%s device not found, retry=%d\r\n", CONFIG_LED_DEV, i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\r\n", CONFIG_LED_DEV);
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\r\n", CONFIG_LED_DEV);
        return;
    }

    ds->led_dev->ops.control(ds->led_dev, DS_CTRL_GET_DEV_INFO, &ds->dev_info);
    ds->buf = kmalloc(ds->dev_info.width * ds->dev_info.height * DS_COLOR_DATA_MAX, GFP_KERNEL);
    if (ds->buf == NULL) {
        pr_err("alloc display buf error\r\n");
        return;
    }
    memset(ds->buf, 0, ds->dev_info.width * ds->dev_info.height * DS_COLOR_DATA_MAX);
    pr_info("display device:[%s] %u*%u\r\n", CONFIG_LED_DEV, ds->dev_info.width, ds->dev_info.height);

    ds->led_enable = true;
    ds->led_dev->ops.control(ds->led_dev, LED_CTRL_ENABLE, NULL);

    device_register(&ds->ds_dev);

    while (true) {
        if (ds->led_enable) {
            display_server_merge_buf(ds);
            mutex_lock(&ds->lock);
            ds->led_dev->ops.write(ds->led_dev, 0, ds->buf,
                ds->dev_info.width * ds->dev_info.height * DS_COLOR_DATA_MAX);
            mutex_unlock(&ds->lock);
            ds->led_dev->ops.control(ds->led_dev, LED_CTRL_REFRESH, NULL);
            msleep(10);
        } else {
            sem_get(&ds->sem);
        }
    }
}

static int display_server_init(void)
{
    struct display_server *ds;

    ds = kmalloc(sizeof(struct display_server), GFP_KERNEL);
    if (ds == NULL) {
        pr_err("alloc display_server buf error\r\n");
        return -ENOMEM;
    }

    sem_init(&ds->sem, 0);
    mutex_init(&ds->lock);
    INIT_LIST_HEAD(&ds->draw_list);

    device_init(&ds->ds_dev);
    ds->ds_dev.name = "display-server";
    ds->ds_dev.ops.control = display_server_control;
    ds->ds_dev.priv = ds;

    ds->task = task_create("display_server", display_server_task_entry, ds, 2, 1024, 10, NULL);
    if (ds->task == NULL) {
        pr_fatal("creat display_server task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ds->task);

    return 0;
}
task_init(display_server_init);
