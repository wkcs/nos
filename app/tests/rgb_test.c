/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[rgb_test]:%s[%d]:"fmt, __func__, __LINE__

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

struct rgb_test {
    struct task_struct *task;
    struct device *ds_dev;
    struct display_server *ds;
    struct ds_draw_info *draw_info;
};

static void rgb_test_draw_info_init(struct rgb_test *rt)
{
    struct ds_draw_area *area;
    struct ds_data *data;
    int i;

    rt->draw_info = display_server_alloc_draw_area_info(rt->ds, 0, 0, 6, 6);
    area = &rt->draw_info->area;

    for (i = 0; i < 36; i++) {
        data = &area->data[i];
        data->data[DS_COLOR_R] = 0;
        data->data[DS_COLOR_G] = 255;
        data->data[DS_COLOR_B] = 0;
    }
}

static void rgb_test_draw_info_update(struct rgb_test *rt)
{
    struct ds_draw_area *area;

    area = &rt->draw_info->area;
    mutex_lock(&rt->draw_info->lock);
    area->x++;
    if (area->x + area->width >= 64) {
        area->x = 0;
        area->y++;
    }
    if (area->y + area->height >= 64) {
        area->y = 0;
    }
    mutex_unlock(&rt->draw_info->lock);
}

static void rgb_test_task_entry(void* parameter)
{
    struct rgb_test *rt = parameter;
    int i;

    rt->ds_dev = NULL;
    for (i = 0; i < 100; i ++) {
        rt->ds_dev = device_find_by_name("display-server");
        if (rt->ds_dev == NULL) {
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

    rt->ds = display_dev_to_server(rt->ds_dev);
    rgb_test_draw_info_init(rt);

    while (true) {
        rgb_test_draw_info_update(rt);
        msleep(100);
    }
}

static int rgb_test_init(void)
{
    struct rgb_test *rt;

    rt = kmalloc(sizeof(struct rgb_test), GFP_KERNEL);
    if (rt == NULL) {
        pr_err("alloc rgb_test buf error\r\n");
        return -ENOMEM;
    }

    rt->task = task_create("rgb_test", rgb_test_task_entry, rt, 20, 1024, 10, NULL);
    if (rt->task == NULL) {
        pr_fatal("creat rgb_test task err\r\n");
        return -EINVAL;
    }
    task_ready(rt->task);

    return 0;
}
task_init(rgb_test_init);
