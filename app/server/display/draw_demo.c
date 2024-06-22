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
#include <string.h>
#include <kernel/sem.h>

#include <display/display.h>

#define LED_NUM 61
#define DEMO_LEN 5

enum {
    INDEX_X,
    INDEX_Y,
    INDEX_MAX
};

static uint8_t index_map[INDEX_MAX][LED_NUM] = {
    {
        0,   1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,
        27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14,
        28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        55, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43,
        56, 57, 58, 59, 66, 67, 68, 69
    },
    {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 4, 4, 4
    }
};

struct draw_demo {
    struct task_struct *task;
    struct device *ds_dev;
    struct display_server *ds;
    struct ds_draw_info *draw_info;
    int index;
};

static void draw_demo_data_init(struct draw_demo *dd)
{
    struct ds_draw_point *point;
    struct ds_data *data;
    int i;

    dd->draw_info = display_server_alloc_draw_point_info(dd->ds, DEMO_LEN);
    point = dd->draw_info->point;
    dd->index = 0;

    mutex_lock(&dd->draw_info->lock);
    for (i = 0; i < DEMO_LEN; i++) {
        data = &point->data;
        point->x = index_map[INDEX_X][i];
        point->y = index_map[INDEX_Y][i];
        switch(i) {
            case 0:
                data->data[DS_COLOR_R] = 5;
                data->data[DS_COLOR_G] = 0;
                data->data[DS_COLOR_B] = 5;
                break;
            case 1:
                data->data[DS_COLOR_R] = 10;
                data->data[DS_COLOR_G] = 0;
                data->data[DS_COLOR_B] = 10;
                break;
            case 2:
                data->data[DS_COLOR_R] = 50;
                data->data[DS_COLOR_G] = 0;
                data->data[DS_COLOR_B] = 50;
                break;
            case 3:
                data->data[DS_COLOR_R] = 150;
                data->data[DS_COLOR_G] = 0;
                data->data[DS_COLOR_B] = 150;
                break;
            case 4:
                data->data[DS_COLOR_R] = 255;
                data->data[DS_COLOR_G] = 0;
                data->data[DS_COLOR_B] = 255;
                break;
            default:
                break;
        }
    }
    mutex_unlock(&dd->draw_info->lock);
}

static void draw_demo_data_update(struct draw_demo *dd)
{
    struct ds_draw_point *point;
    int index;
    int i;

    mutex_lock(&dd->draw_info->lock);
    point = dd->draw_info->point;
    for (i = 0; i < DEMO_LEN; i++) {
        dd->index++;
        if (dd->index >= LED_NUM)
            dd->index = 0;
        index = dd->index + i;
        if (index >= LED_NUM)
            index -= LED_NUM;
        point->x = index_map[INDEX_X][index];
        point->y = index_map[INDEX_Y][index];
    }
    mutex_unlock(&dd->draw_info->lock);
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

    dd->ds = display_dev_to_server(dd->ds_dev);
    draw_demo_data_init(dd);

    while (true) {
        msleep(30);
        draw_demo_data_update(dd);
    }
}

static int draw_demo_init(void)
{
    struct draw_demo *dd;

    dd = kmalloc(sizeof(struct draw_demo), GFP_KERNEL);
    if (dd == NULL) {
        pr_err("alloc draw_demo buf error\r\n");
        return -ENOMEM;
    }

    dd->task = task_create("draw_demo", draw_demo_task_entry, dd, 20, 1024, 10, NULL);
    if (dd->task == NULL) {
        pr_fatal("creat draw_demo task err\r\n");
        return -EINVAL;
    }
    task_ready(dd->task);

    return 0;
}
task_init(draw_demo_init);
