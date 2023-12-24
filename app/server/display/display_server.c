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
#include <lib/string.h>
#include <kernel/sem.h>

#include <display/led.h>
#include <display/display.h>

#define LED_NUM          61

struct display_server {
    struct task_struct *task;
    struct device *led_dev;
    struct device ds_dev;
    sem_t sem;
    struct mutex lock;

    bool led_enable;

    uint8_t buf[LED_NUM * COLOR_MAX];
    uint8_t current_layer[LED_NUM];
};

static ssize_t display_server_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct display_server *ds = dev->priv;
    const uint8_t *buf = buffer;

    if (size != DS_COLOR_DATA_MAX) {
        pr_err("data size error\r\n");
        return -EINVAL;
    }
    if (pos >= LED_NUM) {
        pr_err("index error\r\n");
        return -EINVAL;
    }

    if (buf[DS_COLOR_LAYER] > ds->current_layer[pos])
        return 0;

    ds->current_layer[pos] = buf[DS_COLOR_LAYER];
    if ((buf[DS_COLOR_LAYER] == BACKGROUND_LAYER) && (current != ds->task))
        return 0;

    ds->buf[pos * COLOR_MAX + COLOR_G] = buf[DS_COLOR_G];
    ds->buf[pos * COLOR_MAX + COLOR_B] = buf[DS_COLOR_B];
    ds->buf[pos * COLOR_MAX + COLOR_R] = buf[DS_COLOR_R];

    return size;
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
    case DS_CTRL_CLEAR_LAYER:
        uint8_t index = *((uint8_t *)args);
        if (index >= LED_NUM) {
            pr_err("index to max\r\n");
            return -EINVAL;
        }
        ds->current_layer[index] = BACKGROUND_LAYER;
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void display_server_draw_backgrounp_layer(struct display_server *ds)
{
    static int i = 0, n = 0;
    int m;
    uint8_t data_buf[DS_COLOR_DATA_MAX];

    switch (i) {
    case 0:
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_LOCK, NULL);
        for (m = 0; m < LED_NUM; m++) {
            data_buf[DS_COLOR_R] = 255 - n;
            data_buf[DS_COLOR_G] = n;
            data_buf[DS_COLOR_B] = 0;
            data_buf[DS_COLOR_LAYER] = BACKGROUND_LAYER;
            ds->ds_dev.ops.write(&ds->ds_dev, m, data_buf, DS_COLOR_DATA_MAX);
        }
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_UNLOCK, NULL);
        n += 1;
        if (n == 255)
            i++;
        break;
    case 1:
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_LOCK, NULL);
        for (m = 0; m < LED_NUM; m++) {
            data_buf[DS_COLOR_R] = 0;
            data_buf[DS_COLOR_G] = n;
            data_buf[DS_COLOR_B] = 255 - n;
            data_buf[DS_COLOR_LAYER] = BACKGROUND_LAYER;
            ds->ds_dev.ops.write(&ds->ds_dev, m, data_buf, DS_COLOR_DATA_MAX);
        }
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_UNLOCK, NULL);
        n -= 1;
        if (n == 0)
            i++;
        break;
    case 2:
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_LOCK, NULL);
        for (m = 0; m < LED_NUM; m++) {
            data_buf[DS_COLOR_R] = n;
            data_buf[DS_COLOR_G] = 0;
            data_buf[DS_COLOR_B] = 255 - n;
            data_buf[DS_COLOR_LAYER] = BACKGROUND_LAYER;
            ds->ds_dev.ops.write(&ds->ds_dev, m, data_buf, DS_COLOR_DATA_MAX);
        }
        ds->ds_dev.ops.control(&ds->ds_dev, DS_CTRL_UNLOCK, NULL);
        n += 1;
        if (n == 255) {
            n = 0;
            i = 0;
        }
        break;
    }
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

    memset(ds->buf, 0, LED_NUM * COLOR_MAX);
    memset(ds->current_layer, BACKGROUND_LAYER, LED_NUM);

    ds->led_enable = true;
    ds->led_dev->ops.control(ds->led_dev, LED_CTRL_ENABLE, NULL);

    while (true) {
        if (ds->led_enable) {
            display_server_draw_backgrounp_layer(ds);
            mutex_lock(&ds->lock);
            ds->led_dev->ops.write(ds->led_dev, 0, ds->buf, LED_NUM * COLOR_MAX);
            ds->led_dev->ops.control(ds->led_dev, LED_CTRL_REFRESH, NULL);
            mutex_unlock(&ds->lock);
            msleep(10);
        } else {
            sem_get(&ds->sem);
        }
    }
}

static int display_server_init(void)
{
    struct display_server *ds;

    ds = kalloc(sizeof(struct display_server), GFP_KERNEL);
    if (ds == NULL) {
        pr_err("alloc display_server buf error\r\n");
        return -ENOMEM;
    }

    sem_init(&ds->sem, 0);
    mutex_init(&ds->lock);

    device_init(&ds->ds_dev);
    ds->ds_dev.name = "display-server";
    ds->ds_dev.ops.write = display_server_write;
    ds->ds_dev.ops.control = display_server_control;
    ds->ds_dev.priv = ds;
    device_register(&ds->ds_dev);

    ds->task = task_create("display_server", display_server_task_entry, ds, 2, 10, NULL);
    if (ds->task == NULL) {
        pr_fatal("creat display_server task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ds->task);

    return 0;
}
task_init(display_server_init);
