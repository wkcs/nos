/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[oled_test]:%s[%d]:"fmt, __func__, __LINE__

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

struct oled_test {
    struct task_struct *task;
    struct device *led_dev;
    struct display_dev_info dev_info;
};

static void oled_test_task_entry(void* parameter)
{
    struct oled_test *oled = parameter;
    int i;
    u8 data = 0xff;

    oled->led_dev = NULL;
    for (i = 0; i < 100; i ++) {
        oled->led_dev = device_find_by_name(CONFIG_LED_DEV);
        if (oled->led_dev == NULL) {
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

    oled->led_dev->ops.control(oled->led_dev, DS_CTRL_GET_DEV_INFO, &oled->dev_info);
    pr_info("display device:[%s] %u*%u\r\n", CONFIG_LED_DEV, oled->dev_info.width, oled->dev_info.height);
    oled->led_dev->ops.control(oled->led_dev, DS_CTRL_ENABLE, NULL);

    oled->led_dev->ops.write(oled->led_dev, 0, &data, 1);

}

static int oled_test_init(void)
{
    struct oled_test *oled;

    oled = kmalloc(sizeof(struct oled_test), GFP_KERNEL);
    if (oled == NULL) {
        pr_err("alloc oled_test buf error\r\n");
        return -ENOMEM;
    }

    oled->task = task_create("oled_test", oled_test_task_entry, oled, 2, 1024, 10, NULL);
    if (oled->task == NULL) {
        pr_fatal("creat oled_test task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(oled->task);

    return 0;
}
task_init(oled_test_init);
