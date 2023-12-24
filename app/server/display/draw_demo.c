/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[DRAW_DEMO]:%s[%d]:"fmt, __func__, __LINE__

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

#include <display/display.h>

#define DEMO_LAYER 128
#define DEMO_LEN 5

static uint8_t index_map[61] = {
    0,   1,  2,  3,   4, 5,  6,  7,  8,  9, 10, 11, 12, 13,
    27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41,
    53, 54, 55, 56, 57, 58, 59, 60
};

struct draw_demo_data {
    uint8_t index;
    uint8_t data[DS_COLOR_DATA_MAX];
};

struct draw_demo {
    struct task_struct *task;
    struct device *ds_dev;
    struct draw_demo_data data[DEMO_LEN];
};

static void draw_demo_data_init(struct draw_demo *dd)
{
    dd->data[0].index = 0;
    dd->data[0].data[DS_COLOR_R] = 5;
    dd->data[0].data[DS_COLOR_G] = 0;
    dd->data[0].data[DS_COLOR_B] = 5;
    dd->data[0].data[DS_COLOR_LAYER] = DEMO_LAYER;

    dd->data[1].index = 1;
    dd->data[1].data[DS_COLOR_R] = 10;
    dd->data[1].data[DS_COLOR_G] = 0;
    dd->data[1].data[DS_COLOR_B] = 10;
    dd->data[1].data[DS_COLOR_LAYER] = DEMO_LAYER;

    dd->data[2].index = 2;
    dd->data[2].data[DS_COLOR_R] = 50;
    dd->data[2].data[DS_COLOR_G] = 0;
    dd->data[2].data[DS_COLOR_B] = 50;
    dd->data[2].data[DS_COLOR_LAYER] = DEMO_LAYER;

    dd->data[3].index = 3;
    dd->data[3].data[DS_COLOR_R] = 150;
    dd->data[3].data[DS_COLOR_G] = 0;
    dd->data[3].data[DS_COLOR_B] = 150;
    dd->data[3].data[DS_COLOR_LAYER] = DEMO_LAYER;

    dd->data[4].index = 4;
    dd->data[4].data[DS_COLOR_R] = 255;
    dd->data[4].data[DS_COLOR_G] = 0;
    dd->data[4].data[DS_COLOR_B] = 255;
    dd->data[4].data[DS_COLOR_LAYER] = DEMO_LAYER;
}

static void draw_demo_data_update(struct draw_demo *dd)
{
    static int i = 4;
    int m, n;

    if (i > 60)
        i = 0;
    else
        i++;

    dd->ds_dev->ops.control(dd->ds_dev, DS_CTRL_CLEAR_LAYER, &dd->data[0].index);
    for (m = 0; m < DEMO_LEN; m++) {
        dd->data[m].index = index_map[(i + 61 + m - 4) % 61];
    }
}

static void draw_demo_task_entry(void* parameter)
{
    struct draw_demo *dd = parameter;
    int i;

    dd->ds_dev = NULL;
    for (i = 0; i < 100; i ++) {
        dd->ds_dev = device_find_by_name("display-server");
        if (dd->ds_dev == NULL) {
            i++;
            pr_err("%s device not found, retry=%d\r\n", "display-server", i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\r\n", "display-server");
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\r\n", "display-server");
        return;
    }

    draw_demo_data_init(dd);

    while (true) {
        dd->ds_dev->ops.control(dd->ds_dev, DS_CTRL_LOCK, NULL);
        for (i = 0; i < DEMO_LEN; i++) {
            dd->ds_dev->ops.write(dd->ds_dev, dd->data[i].index, dd->data[i].data, DS_COLOR_DATA_MAX);
        }
        dd->ds_dev->ops.control(dd->ds_dev, DS_CTRL_UNLOCK, NULL);

        msleep(30);
        draw_demo_data_update(dd);
    }
}

static int draw_demo_init(void)
{
    struct draw_demo *dd;

    dd = kalloc(sizeof(struct draw_demo), GFP_KERNEL);
    if (dd == NULL) {
        pr_err("alloc draw_demo buf error\r\n");
        return -ENOMEM;
    }

    dd->task = task_create("draw_demo", draw_demo_task_entry, dd, 20, 10, NULL);
    if (dd->task == NULL) {
        pr_fatal("creat draw_demo task err\r\n");
        return -EINVAL;
    }
    task_ready(dd->task);

    return 0;
}
task_init(draw_demo_init);
