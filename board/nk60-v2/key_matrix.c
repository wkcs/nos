/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V2 74HC595 Shift-Register Key Matrix Hardware Driver ("nk60_v2-key").
 * Scans HC595_BIT_NUM bits, maps raw hardware scan bits to logical key indices
 * using driver-internal position mapping, and outputs standard logical key bitmaps.
 */

#define pr_fmt(fmt) "[KEY]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <string.h>
#include <board/board.h>
#include <nos/keyboard.h>

#define HC595_EN   PBout(12)
#define HC595_CLK  PBout(13)
#define HC595_DATA PBout(15)
#define KEY_DATA   PBin(14)

#define KEY_UP   1
#define KEY_DOWN 0

#define NK60_KEY_NUM        61U
#define HC595_BIT_NUM       (8 * 9)
#define HC595_RAW_DATA_NUM  KEY_RAW_WORDS(HC595_BIT_NUM)
#define NK60_KEY_WORDS      KEY_RAW_WORDS(NK60_KEY_NUM)

/* 74HC595 硬件扫描位到 NK60 逻辑按键序号的私有走线映射表 */
static const int8_t s_nk60_position_index[HC595_BIT_NUM] = {
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, -1, -1,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, -1, -1,
    36, 37, 38, 39, 40, -1, -1, -1,
    49, 50, 51, 52, -1, -1, -1, -1,
    53, 54, 55, 56, 57, 58, 59, 60,
    41, 42, 43, 44, 45, 46, 47, 48,
    28, 29, 30, 31, 32, 33, 34, 35
};

struct nk60_key {
    struct mutex            lock;
    struct device           dev;
    bool                    enable;
    uint16_t                debounce_ms;
    uint16_t                poll_rate_hz;
    struct key_driver_stats stats;
};

static void hc595_load(void)
{
    HC595_EN = 1;
    usleep(1);
    HC595_EN = 0;
    usleep(10);
}

static void hc595_send_bit(uint8_t data)
{
    HC595_DATA = data;
    usleep(1);
    HC595_CLK = 1;
    usleep(1);
    HC595_CLK = 0;
    hc595_load();
}

static void nk60_v2_key_scan_once(uint32_t *raw_data)
{
    int i;

    memset(raw_data, 0, HC595_RAW_DATA_NUM * sizeof(uint32_t));

    for (i = 0; i < HC595_BIT_NUM; i++) {
        hc595_send_bit(i != 0);
        if (KEY_DATA == KEY_DOWN) {
            raw_data[KEY_RAW_GROUP(i)] |= (1UL << KEY_RAW_BIT(i));
        }
    }
}

static ssize_t nk60_v2_key_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    struct nk60_key *key = dev->priv;
    uint32_t raw_data[HC595_RAW_DATA_NUM];
    uint32_t *key_bitmap = (uint32_t *)buffer;
    size_t copy_bytes = NK60_KEY_WORDS * sizeof(uint32_t);
    int bit;
    int w;

    (void)pos;
    if (key == NULL || buffer == NULL || size < copy_bytes) {
        return -EINVAL;
    }
    if (!key->enable) {
        memset(buffer, 0, copy_bytes);
        return (ssize_t)copy_bytes;
    }

    mutex_lock(&key->lock);
    nk60_v2_key_scan_once(raw_data);
    key->stats.scan_count++;

    /* 在驱动内部将 74HC595 硬件扫描位转换为规范的逻辑按键位图 */
    memset(key_bitmap, 0, copy_bytes);
    for (bit = 0; bit < HC595_BIT_NUM; bit++) {
        if ((raw_data[KEY_RAW_GROUP(bit)] >> KEY_RAW_BIT(bit)) & 0x01UL) {
            int8_t key_idx = s_nk60_position_index[bit];
            if (key_idx >= 0 && (uint8_t)key_idx < NK60_KEY_NUM) {
                key_bitmap[KEY_RAW_GROUP((uint8_t)key_idx)] |=
                    (1UL << KEY_RAW_BIT((uint8_t)key_idx));
            }
        }
    }

    for (w = 0; w < (int)NK60_KEY_WORDS; w++) {
        if (key_bitmap[w] != 0U) {
            key->stats.key_down_events++;
            break;
        }
    }
    mutex_unlock(&key->lock);
    return (ssize_t)copy_bytes;
}

static int nk60_v2_key_control(struct device *dev, int cmd, void *args)
{
    struct nk60_key *key = dev->priv;
    struct key_dev_info info;

    if (key == NULL) {
        return -EINVAL;
    }

    switch (cmd) {
    case KEY_CTRL_ENABLE:
        key->enable = true;
        break;
    case KEY_CTRL_DISABLE:
        key->enable = false;
        HC595_CLK = 0;
        HC595_DATA = 0;
        HC595_EN = 0;
        break;
    case KEY_CTRL_GET_ENABLE_STATUS:
        return key->enable;
    case KEY_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            return -EINVAL;
        }
        info.key_count = NK60_KEY_NUM;
        info.debounce_ms = key->debounce_ms;
        info.poll_rate_hz = key->poll_rate_hz;
        memcpy(args, &info, sizeof(info));
        break;
    case KEY_CTRL_SET_DEBOUNCE:
        if (args == NULL) {
            return -EINVAL;
        }
        key->debounce_ms = *(const uint16_t *)args;
        break;
    case KEY_CTRL_SET_POLL_RATE:
        if (args == NULL) {
            return -EINVAL;
        }
        key->poll_rate_hz = *(const uint16_t *)args;
        break;
    case KEY_CTRL_GET_STATS:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, &key->stats, sizeof(struct key_driver_stats));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static int nk60_v2_key_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    HC595_CLK = 0;
    HC595_DATA = 0;
    HC595_EN = 0;

    return 0;
}

static int nk60_v2_key_init(void)
{
    struct nk60_key *key;

    key = kmalloc(sizeof(struct nk60_key), GFP_KERNEL);
    if (key == NULL) {
        pr_err("alloc nk60_key error\r\n");
        return -ENOMEM;
    }

    nk60_v2_key_gpio_init();
    mutex_init(&key->lock);
    key->enable = true;
    key->debounce_ms = 10U;
    key->poll_rate_hz = 100U;
    memset(&key->stats, 0, sizeof(key->stats));

    device_init(&key->dev);
    key->dev.name = "nk60_v2-key";
    key->dev.ops.read = nk60_v2_key_read;
    key->dev.ops.control = nk60_v2_key_control;
    key->dev.priv = key;
    device_register(&key->dev);

    return 0;
}
task_init(nk60_v2_key_init);
