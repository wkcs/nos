/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[LED]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <board/board.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>

#include <display/led.h>
#include <display/display.h>

#define LED_WIDTH 64
#define LED_HEIGHT 64
#define LED_BUF_SZIE (LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX)

#define RGB_R1(value)  (PCout(0) = !!(value))
#define RGB_G1(value)  (PCout(1) = !!(value))
#define RGB_B1(value)  (PCout(2) = !!(value))

#define RGB_R2(value)  (PCout(3) = !!(value))
#define RGB_G2(value)  (PAout(1) = !!(value))
#define RGB_B2(value)  (PCout(6) = !!(value))

#define RGB_A(value)  (PAout(5) = !!(value))
#define RGB_B(value)  (PAout(6) = !!(value))
#define RGB_C(value)  (PAout(7) = !!(value))
#define RGB_D(value)  (PCout(4) = !!(value))
#define RGB_E(value)  (PAout(4) = !!(value))

#define RGB_CLK(value)  (PCout(5) = !!(value))
#define RGB_LAT(value)  (PCout(7) = !!(value))
#define RGB_OE(value)   (PCout(8) = !!(value))

struct rgb_matrix {
    struct task_struct *task;
    struct mutex lock;
    struct device dev;
    uint8_t buf[LED_BUF_SZIE];

    bool enable;
};

void rgb_matrix_display(struct rgb_matrix *led)
{
    int i, j;

    for (j = 0; j < 32; j++) {
        for (i = 0; i < LED_WIDTH; i++) {
            RGB_R1(led->buf[(i + (j * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_R]);
            RGB_G1(led->buf[(i + (j * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_G]);
            RGB_B1(led->buf[(i + (j * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_B]);

            RGB_R2(led->buf[(i + ((j + 32) * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_R]);
            RGB_G2(led->buf[(i + ((j + 32) * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_G]);
            RGB_B2(led->buf[(i + ((j + 32) * LED_WIDTH)) * DS_COLOR_DATA_MAX + DS_COLOR_B]);
            
            RGB_CLK(0); //data input
            RGB_CLK(1); 
        }

        RGB_A(j & 0x1);
        RGB_B(j & 0x2);
        RGB_C(j & 0x4);
        RGB_D(j & 0x8);
        RGB_E(j & 0x10);

        RGB_LAT(1);
        RGB_LAT(0);
        
        RGB_OE(0);
        usleep(30);
        RGB_OE(1);
    }
}

static void rgb_matrix_buf_init(struct rgb_matrix *led)
{
    mutex_lock(&led->lock);
    memset(led->buf, 0, LED_BUF_SZIE);
    mutex_unlock(&led->lock);
}

static ssize_t rgb_matrix_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct rgb_matrix *led = dev->priv;

    if (size != LED_BUF_SZIE) {
        pr_err("data size error\r\n");
        return -EINVAL;
    }

    mutex_lock(&led->lock);
    memcpy(led->buf, buffer, LED_BUF_SZIE);
    mutex_unlock(&led->lock);

    return size;
}

static int rgb_matrix_control(struct device *dev, int cmd, void *args)
{
    struct rgb_matrix *led = dev->priv;
    struct display_dev_info info = {.width = LED_WIDTH, .height = LED_HEIGHT};

    switch (cmd) {
    case LED_CTRL_ENABLE:
        led->enable = true;
        break;
    case LED_CTRL_DISABLE:
        led->enable = false;
        rgb_matrix_buf_init(led);
        break;
    case LED_CTRL_GET_ENABLE_STATUS:
        return led->enable;
    case LED_CTRL_REFRESH:
        break;
    case DS_CTRL_GET_DEV_INFO:
        memcpy(args, &info, sizeof(struct display_dev_info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static int rgb_matrix_gpio_init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOE, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOE, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOE, GPIO_Pin_6);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOA, GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7 | GPIO_Pin_8;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOC, GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_6 | GPIO_Pin_7);
    GPIO_SetBits(GPIOC, GPIO_Pin_5 | GPIO_Pin_8);

    RGB_CLK(1);
    RGB_OE(1);

    return 0;
}

static void rgb_matrix_task_entry(void* parameter)
{
    struct rgb_matrix *led = parameter;

    while(true) {
        mutex_lock(&led->lock);
        rgb_matrix_display(led);
        mutex_unlock(&led->lock);
        msleep(10);
    }
}

static int rgb_matrix_init(void)
{
    struct rgb_matrix *led;

    led = kmalloc(sizeof(struct rgb_matrix), GFP_KERNEL);
    if (led == NULL) {
        pr_err("alloc rgb_matrix buf error\r\n");
        return -ENOMEM;
    }

    rgb_matrix_gpio_init();
    memset(led->buf, 0, LED_BUF_SZIE);

    mutex_init(&led->lock);
    led->enable = false;

    device_init(&led->dev);
    led->dev.name = "rgb-matrix";
    led->dev.ops.write = rgb_matrix_buf_write;
    led->dev.ops.control = rgb_matrix_control;
    led->dev.priv = led;
    device_register(&led->dev);

    led->task = task_create("rgb-matrix", rgb_matrix_task_entry, led, 10, 512, 10, NULL);
    if (led->task == NULL) {
        pr_fatal("creat rgb_matrix task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(led->task);

    return 0;
}
task_init(rgb_matrix_init);
