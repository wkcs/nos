/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[lcd_test]:%s[%d]:"fmt, __func__, __LINE__

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

struct lcd_test {
    struct task_struct *task;
    struct device *led_dev;
    struct display_dev_info dev_info;
};

#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define BRED    0XF81F
#define GRED    0XFFE0
#define GBLUE   0X07FF
#define RED     0xF800
#define MAGENTA 0xF81F
#define GREEN   0x07E0
#define CYAN    0x7FFF
#define YELLOW  0xFFE0
#define BROWN   0XBC40 // 棕色
#define BRRED   0XFC07 // 棕红色
#define GRAY    0X8430 // 灰色

static u16 lcd_test_data[] = {
    WHITE, BLACK, BLUE, BRED, GRED, GBLUE, RED,
    MAGENTA, GREEN, CYAN, YELLOW, BROWN, BRRED,
    GRAY
};

static void lcd_test_lcd_clear(struct lcd_test *lcd, u16 data)
{
    int x, y;

    for (y = 0; y < lcd->dev_info.height; y++) {
        for (x = 0; x < lcd->dev_info.width; x++) {
            lcd->led_dev->ops.write(lcd->led_dev, y * lcd->dev_info.width + x, &data, 2);
        }
    }
}

static void lcd_test_task_entry(void* parameter)
{
    struct lcd_test *lcd = parameter;
    int i;

    lcd->led_dev = NULL;
    for (i = 0; i < 100; i ++) {
        lcd->led_dev = device_find_by_name(CONFIG_LED_DEV);
        if (lcd->led_dev == NULL) {
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

    lcd->led_dev->ops.control(lcd->led_dev, DS_CTRL_GET_DEV_INFO, &lcd->dev_info);
    pr_info("display device:[%s] %u*%u\r\n", CONFIG_LED_DEV, lcd->dev_info.width, lcd->dev_info.height);
    lcd->led_dev->ops.control(lcd->led_dev, DS_CTRL_ENABLE, NULL);

    i = 0;
    while (true) {
        if (i >= sizeof(lcd_test_data) / sizeof(lcd_test_data[0])) {
            i = 0;
        }
        lcd_test_lcd_clear(lcd, lcd_test_data[i]);
        i++;
        sleep(1);
    }
}

static int lcd_test_init(void)
{
    struct lcd_test *lcd;

    lcd = kmalloc(sizeof(struct lcd_test), GFP_KERNEL);
    if (lcd == NULL) {
        pr_err("alloc lcd_test buf error\r\n");
        return -ENOMEM;
    }

    lcd->task = task_create("lcd_test", lcd_test_task_entry, lcd, 2, 1024, 10, NULL);
    if (lcd->task == NULL) {
        pr_fatal("creat lcd_test task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(lcd->task);

    return 0;
}
task_init(lcd_test_init);
