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
#include <lib/string.h>
#include <board/board.h>

#define LED_VCC_EN PBout(0)
#define LED_DATA_1 PBout(6)
#define LED_DATA_2 PBout(7)
#define LED_DATA_3 PBout(8)
#define LED_DATA_4 PBout(9)
#define LED_DATA_5 PBout(10)
#define LED_DATA_6 PBout(12)

enum {
    COLOR_R = 0,
    COLOR_G,
    COLOR_B,
    COLOR_MAX,
};
#define LED_NUM 61

static uint8_t g_led_buf[LED_NUM][COLOR_MAX];

#define DELAY_11_9_NS __NOP()
#define DELAY_80_NS DELAY_11_9_NS;DELAY_11_9_NS;DELAY_11_9_NS;DELAY_11_9_NS;DELAY_11_9_NS;DELAY_11_9_NS;DELAY_11_9_NS

struct nk60_led {
    struct task_struct *task;
    spinlock_t lock;
    struct device dev;
    uint8_t *buf;
};

static void nk60_v2_led_reset(void)
{
    LED_DATA_1 = 0;
    usleep(90);
}

static void nk60_v2_led_write_bit(bool high)
{
    LED_DATA_1 = 1;
    if (high) {
        nsleep(400);
        LED_DATA_1 = 0;
        nsleep(800);
    } else {
        DELAY_80_NS;
        LED_DATA_1 = 0;
        usleep(1);
    }
}

static void nk60_v2_led_write_byte(uint8_t data)
{
    int i;

    for (i = 0; i < 8; i++) {
        nk60_v2_led_write_bit(data & 0x80);
        data <<= 1;
    }
}

static void nk60_v2_led_write_rgb_data(uint8_t r, uint8_t g, uint8_t b)
{
    nk60_v2_led_write_byte(g);
    nk60_v2_led_write_byte(r);
    nk60_v2_led_write_byte(b);
}

ssize_t nk60_v2_led_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct nk60_led *led = dev->priv;

    if (size != LED_NUM * COLOR_MAX) {
        pr_err("data size error\r\n");
        return -EINVAL;
    }

    spin_lock(&led->lock);
    memcpy(led->buf, buffer, size);
    spin_unlock(&led->lock);

    return size;
}

static void nk60_v2_led_refresh(struct nk60_led *led)
{
    int i;

    spin_lock(&led->lock);
    for (i = 0; i < LED_NUM; i++)
        nk60_v2_led_write_rgb_data(
            led->buf[i * COLOR_MAX + COLOR_R],
            led->buf[i * COLOR_MAX + COLOR_G],
            led->buf[i * COLOR_MAX + COLOR_B]);
    spin_unlock(&led->lock);
}

static void nk60_v2_led_task_entry(void* parameter)
{
    struct nk60_led *led = parameter;
    uint8_t i = 0, n = 0;
    int m;

    while (true) {
        switch (i) {
        case 0:
            for (m = 0; m < LED_NUM; m++) {
                led->buf[m * COLOR_MAX + COLOR_G] = n;
                led->buf[m * COLOR_MAX + COLOR_R] = 255 - n;
            }
            n += 1;
            if (n == 255)
                i++;
            break;
        case 1:
            for (m = 0; m < LED_NUM; m++) {
                led->buf[m * COLOR_MAX + COLOR_G] = n;
                led->buf[m * COLOR_MAX + COLOR_B] = 255 - n;
            }
            n -= 1;
            if (n == 0)
                i++;
            break;
        case 2:
            for (m = 0; m < LED_NUM; m++) {
                led->buf[m * COLOR_MAX + COLOR_R] = n;
                led->buf[m * COLOR_MAX + COLOR_B] = 255 - n;
            }
            n += 1;
            if (n == 255) {
                n = 0;
                i = 0;
            }
            break;
        }
        nk60_v2_led_refresh(led);
        msleep(20);
    }
}

static int nk60_v2_led_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin =  GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_12;;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    LED_VCC_EN = 1;
    LED_DATA_1 = 0;
    LED_DATA_2 = 0;
    LED_DATA_3 = 0;
    LED_DATA_4 = 0;
    LED_DATA_5 = 0;
    LED_DATA_6 = 0;

    return 0;
}

static int nk60_v2_led_init(void)
{
    struct nk60_led *led;

    led = kalloc(sizeof(struct nk60_led), GFP_KERNEL);
    if (led == NULL) {
        pr_err("alloc led buf error\r\n");
        return -ENOMEM;
    }

    nk60_v2_led_gpio_init();

    led->buf = (uint8_t *)g_led_buf;
    spin_lock_init(&led->lock);

    device_init(&led->dev);
    led->dev.name = "nk60-led";
    led->dev.ops.write = nk60_v2_led_buf_write;
    led->dev.priv = led;
    device_register(&led->dev);

    led->task = task_create("nk60_v2-led", nk60_v2_led_task_entry, led, 2, 10, NULL);
    if (led->task == NULL) {
        pr_fatal("creat nk60_v2-led task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(led->task);

    return 0;
}
task_init(nk60_v2_led_init);
