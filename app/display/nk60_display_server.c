/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[NK60-DISPLAY]:%s[%d]:"fmt, __func__, __LINE__

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
#include <kernel/console.h>

#include <display/display.h>

#define LED_WIDTH  14
#define LED_HEIGHT 5

struct nk60_display_server {
    struct task_struct *task;
    struct device *ds_dev;
    struct display_server *ds;
    struct ds_draw_info *draw_info;
};

#define GRADATION_DATA_MAX 24
static u8 g_gradation_rgb_data[GRADATION_DATA_MAX][DS_COLOR_DATA_MAX] = {
    {0, 230, 18}, {97, 235, 0}, {152, 243, 0}, {200, 252, 0}, {251, 255, 0},
    {219, 207, 0}, {195, 143, 31}, {172, 34, 56}, {153, 0, 68}, {155, 0, 107},
    {158, 0, 150}, {160, 0, 193}, {160, 0, 233}, {134, 0, 209}, {104, 0, 183},
    {71, 0, 157}, {32, 29, 136}, {25, 96, 134}, {7, 146, 131}, {0, 190, 129},
    {0, 228, 127}, {0, 229, 106}, {0, 229, 79}, {0, 230, 51}
};

static int nk60_display_server_draw_info_init(struct nk60_display_server *nk60_ds)
{
    struct ds_draw_area *area;
    struct ds_data *data;
    int i;

    nk60_ds->draw_info = display_server_alloc_draw_area_info(nk60_ds->ds,
        0, 0, LED_WIDTH, LED_HEIGHT);
    if (nk60_ds->draw_info == NULL) {
        pr_err("alloc draw_info buf error\r\n");
        return -ENOMEM;
    }
    area = &nk60_ds->draw_info->area;

    for (i = 0; i < LED_WIDTH * LED_HEIGHT; i++) {
        data = &area->data[i];
        data->data[DS_COLOR_R] = 0;
        data->data[DS_COLOR_G] = 0;
        data->data[DS_COLOR_B] = 0;
    }

    return 0;
}

static void nk60_display_server_data_update(struct nk60_display_server *nk60_ds)
{
    struct ds_draw_area *area;
    struct ds_data *data;
    int i;
    static int index = GRADATION_DATA_MAX;
    int tmp_index;

    area = &nk60_ds->draw_info->area;
    index--;
    if (index < 0)
        index = GRADATION_DATA_MAX - 1;

    for (i = 0; i < LED_WIDTH * LED_HEIGHT; i++) {
        tmp_index = (index + (i % LED_WIDTH)) % GRADATION_DATA_MAX;
        data = &area->data[i];
        data->data[DS_COLOR_R] = g_gradation_rgb_data[tmp_index][DS_COLOR_R];
        data->data[DS_COLOR_G] = g_gradation_rgb_data[tmp_index][DS_COLOR_G];
        data->data[DS_COLOR_B] = g_gradation_rgb_data[tmp_index][DS_COLOR_B];
    }
}

static void nk60_display_server_task_entry(void* parameter)
{
    struct nk60_display_server *nk60_ds = parameter;
    int i;
    int rc;

    nk60_ds->ds_dev = NULL;
    for (i = 0; i < 100; i ++) {
        nk60_ds->ds_dev = device_find_by_name("display-server");
        if (nk60_ds->ds_dev == NULL) {
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

    nk60_ds->ds = display_dev_to_server(nk60_ds->ds_dev);
    rc = nk60_display_server_draw_info_init(nk60_ds);
    if (rc < 0) {
        pr_err("init draw_info buf error\r\n");
        return;
    }

    while (true) {
        msleep(30);
        nk60_display_server_data_update(nk60_ds);
    }
}

static int nk60_display_server_init(void)
{
    struct nk60_display_server *nk60_ds;

    nk60_ds = kmalloc(sizeof(struct nk60_display_server), GFP_KERNEL);
    if (nk60_ds == NULL) {
        pr_err("alloc nk60_display_server buf error\r\n");
        return -ENOMEM;
    }

    nk60_ds->task = task_create("nk60_display_server",
        nk60_display_server_task_entry, nk60_ds, 20, 1024, 10, NULL);
    if (nk60_ds->task == NULL) {
        pr_fatal("creat nk60_display_server task err\r\n");

        return -EINVAL;
    }
    task_ready(nk60_ds->task);

    return 0;
}
task_init(nk60_display_server_init);
