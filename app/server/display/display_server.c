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

#include <led/led.h>

#define LED_NUM 61

struct display_server {
    struct task_struct *task;
    struct device *led_dev;
    uint8_t *buf;

    uint32_t led_enable;
};

static void display_server_task_entry(void* parameter)
{
    struct display_server *ds = parameter;
    uint8_t i = 0, n = 0;
    int m;

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

    ds->buf = kalloc(LED_NUM * COLOR_MAX, GFP_KERNEL);
    if (ds->buf == NULL) {
        pr_err("alloc led buf error, exit\r\n");
        return;
    }
    memset(ds->buf, 0, LED_NUM * COLOR_MAX);

    ds->led_enable = 1;
    ds->led_dev->ops.control(ds->led_dev, LED_CTRL_ENABLE, (void *)ds->led_enable);

    while (true) {
        switch (i) {
        case 0:
            for (m = 0; m < LED_NUM; m++) {
                ds->buf[m * COLOR_MAX + COLOR_G] = n;
                ds->buf[m * COLOR_MAX + COLOR_R] = 255 - n;
            }
            n += 1;
            if (n == 255)
                i++;
            break;
        case 1:
            for (m = 0; m < LED_NUM; m++) {
                ds->buf[m * COLOR_MAX + COLOR_G] = n;
                ds->buf[m * COLOR_MAX + COLOR_B] = 255 - n;
            }
            n -= 1;
            if (n == 0)
                i++;
            break;
        case 2:
            for (m = 0; m < LED_NUM; m++) {
                ds->buf[m * COLOR_MAX + COLOR_R] = n;
                ds->buf[m * COLOR_MAX + COLOR_B] = 255 - n;
            }
            n += 1;
            if (n == 255) {
                n = 0;
                i = 0;
            }
            break;
        }

        ds->led_dev->ops.write(ds->led_dev, 0, ds->buf, LED_NUM * COLOR_MAX);
        msleep(10);
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
