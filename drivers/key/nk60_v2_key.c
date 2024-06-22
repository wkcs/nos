/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[KEY]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <string.h>
#include <board/board.h>
#include "keyboard.h"

#define HC595_EN   PBout(12)
#define HC595_CLK  PBout(13)
#define HC595_DATA PBout(15)
#define KEY_DATA   PBin(14)

#define KEY_NUM 61
#define KEY_RAW_DATA_NUM ((KEY_NUM) / 32 + (((KEY_NUM) % 32) ? 1 : 0))
#define KEY_RAW_DATA_GROUP(index) ((index) / 32)
#define KEY_RAW_DATA_INDEX(index) ((index) % 32)

#define KEY_UP   1
#define KEY_DOWN 0

#define DEF_LAYER 0
#define FN_LAYER  1
#define PN_LAYER  2

#define HC595_BIT_NUM (8 * 9)

static int def_key_code_layout[3][KEY_NUM] = {
    KEYMAP(
        ESC, 1,   2,   3,   4,   5,   6,   7,   8,   9,   0,   MINS,EQL, BSPC, \
        TAB, Q,   W,   E,   R,   T,   Y,   U,   I,   O,   P,   LBRC,RBRC,BSLS, \
        CAPS,A,   S,   D,   F,   G,   H,   J,   K,   L,   SCLN,QUOT,     ENT,  \
        LSFT,Z,   X,   C,   V,   B,   N,   M,   COMM,DOT, SLSH,          RSFT, \
        LCTL,LGUI,LALT,          SPC,                     RALT, PN, RCTL,FN    ),
    KEYMAP(
        GRV, F1,  F2,  F3,  F4,  F5,  F6,  F7,  F8,  F9,  F10, F11, F12, DEL, \
        NO,  NO , UP,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  INS, \
        NO,  LEFT,DOWN,RGHT,NO,  NO,  PSCR,SLCK,PAUS,NO,  NO,  END,      NO,  \
        NO,  NO , NO,  NO,  NO,  NO,  NO,  NO,  PGUP,PGDN,UP,            NO,  \
        NO,  NO,  NO,            NO,                      LEFT,DOWN,RGHT,NO   ),
    KEYMAP(
        GRV, F1,  F2,  F3,  F4,  F5,  F6,  F7,  F8,  F9,  F10, F11, F12, DEL, \
        NO,  NO , UP,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  INS, \
        NO,  LEFT,DOWN,RGHT,NO,  NO,  PSCR,SLCK,PAUS,NO,  NO,  END,      NO,  \
        NO,  NO , NO,  NO,  NO,  NO,  NO,  NO,  NO  ,NO  ,NO,            NO,  \
        NO,  NO,  NO,            NO,                      NO,  NO,  NO,  NO   ),
};

static int def_action_layout[3][KEY_NUM] = {
    ACTION_MAP(
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        CAPS,NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,       NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,            NO,   \
        NO,  NO,  NO,            NO,                      NO,  NO,  NO,  NO    ),
    ACTION_MAP(
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,       NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,            NO,   \
        NO,  NO,  NO,            NO,                      NO,  NO,  NO,  NO    ),
    ACTION_MAP(
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,       NO,   \
        NO,  NO,  NO,  NO,  NO,  NO,  LED, NO,  NO,  NO,  NO,            NO,   \
        NO,  NO,  NO,            NO,                      NO,  NO,  NO,  NO    ),
};

static struct key_info def_key_info[KEY_NUM];

static struct task_struct *key_task;
static struct device *g_hid_dev;

static int position_index[HC595_BIT_NUM] = {
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

static void hc595_load(void)
{
    HC595_EN = 1;
	usleep(1);
    HC595_EN = 0;
	usleep(10);
}

static void hc595_send(uint8_t data)
{
    uint8_t i;

    for (i = 8; i > 0; i--) {
        if(data & 0x1)
            HC595_DATA = 1;
        else
            HC595_DATA = 0;
        usleep(1);
        HC595_CLK = 1;
        usleep(1);
        data >>= 1;
        HC595_CLK = 0;
    }
    hc595_load();
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

static void nk60_v2_key_scan(uint32_t *raw_data)
{
    uint32_t raw_data_old[KEY_RAW_DATA_NUM] = { 0 };
    int i;
    int index;

    memset(raw_data, 0, KEY_RAW_DATA_NUM * sizeof(uint32_t));

    for (i = 0; i < HC595_BIT_NUM; i++) {
        hc595_send_bit(i != 0);
        index = position_index[i];
        if (index < 0)
            continue;
        if (KEY_DATA == KEY_DOWN)
            raw_data_old[KEY_RAW_DATA_GROUP(index)] |= 1 << KEY_RAW_DATA_INDEX(index);
    }
    msleep(10);
    for (i = 0; i < HC595_BIT_NUM; i++) {
        hc595_send_bit(i != 0);
        index = position_index[i];
        if (index < 0)
            continue;
        if (KEY_DATA == KEY_DOWN)
            raw_data[KEY_RAW_DATA_GROUP(index)] |= 1 << KEY_RAW_DATA_INDEX(index);
    }

    for (index = 0; index < KEY_RAW_DATA_NUM; index++) {
        raw_data[index] &= raw_data_old[index];
        // pr_info("data[%d]=0x%x\r\n", index, raw_data[index]);
    }
}

static int nk60_v2_get_key_layer(uint32_t *raw_data, struct key_info *old_info, int *special_key_layout)
{
    int layer = DEF_LAYER;
    int i;

    for (i = 0; i < KEY_NUM; i++) {
        if (IS_FN(special_key_layout[i]) && ((raw_data[KEY_RAW_DATA_GROUP(i)] >> KEY_RAW_DATA_INDEX(i)) & 0x01)) {
            if (old_info[i].code == KC_FN)
                return FN_LAYER;
            layer = FN_LAYER;
        }
        if (IS_PN(special_key_layout[i]) && ((raw_data[KEY_RAW_DATA_GROUP(i)] >> KEY_RAW_DATA_INDEX(i)) & 0x01)) {
            if (old_info[i].code == KC_PN)
                return PN_LAYER;
            if (layer == DEF_LAYER) /* FN和PN同时按下优先使用FN */
                layer = PN_LAYER;
        }
    }

    return layer;
}

static void nk60_v2_key_info_update(uint32_t *raw_data, struct key_info *info)
{
    int i;
    int layer;

    layer = nk60_v2_get_key_layer(raw_data, info, def_key_code_layout[DEF_LAYER]);
    for (i = 0; i < KEY_NUM; i++) {
        if ((raw_data[KEY_RAW_DATA_GROUP(i)] >> KEY_RAW_DATA_INDEX(i)) & 0x01) {
            if (info[i].val != KEY_DOWN) {
                info[i].code = def_key_code_layout[layer][i];
                info[i].val = KEY_DOWN;
                info[i].action = def_action_layout[layer][i];
                pr_info("key down: layer=%d, index=%d, code=0x%02x, action=%d\r\n", layer, i, info[i].code, info[i].action);
                if (((layer == FN_LAYER) && (def_key_code_layout[DEF_LAYER][i] == KC_FN)) ||
                    ((layer == PN_LAYER) && (def_key_code_layout[DEF_LAYER][i] == KC_PN))) {
                    info[i].code = def_key_code_layout[DEF_LAYER][i];
                    info[i].action = def_action_layout[DEF_LAYER][i];
                }
            }
        } else {
            info[i].code = KC_NO;
            info[i].val = KEY_UP;
            info[i].action = KA_NO;
        }
    }
}

static void nk60_v2_hid_data_update(struct key_info *info, uint8_t *hid_data)
{
    int i, n, m;
    uint8_t data_bck[6] = { 0 };
    uint8_t data_new[6] = { 0 };
    int bck_num, new_num;
    bool old_data;

    hid_data[0] = 0;
    bck_num = 0;
    new_num = 0;
    for (i = 0; i < 64; i++) {
        if (info[i].val != KEY_DOWN)
            continue;
        switch (info[i].code) {
        case KC_LSHIFT:
            hid_data[0] |= (uint8_t)(1 << 1);
            break;
        case KC_RSHIFT:
            hid_data[0] |= (uint8_t)(1 << 5);
            break;
        case KC_LCTRL:
            hid_data[0] |= (uint8_t)1;
            break;
        case KC_LGUI:
            hid_data[0] |= (uint8_t)(1 << 3);
            break;
        case KC_RGUI:
            hid_data[0] |= (uint8_t)(1 << 7);
            break;
        case KC_LALT:
            hid_data[0] |= (uint8_t)(1 << 2);
            break;
        case KC_RALT:
            hid_data[0] |= (uint8_t)(1 << 6);
            break;
        case KC_RCTRL:
            hid_data[0] |= (uint8_t)(1 << 4);
            break;
        case KC_FN:
        case KC_PN:
        case KC_NO:
            break;
        default:
            old_data = false;
            for (n = 2; n < 8; n++) {
                if (hid_data[n] == KC_NO)
                    continue;
                if (hid_data[n] == info[i].code) {
                    old_data = true;
                    for (m = 0; m < bck_num; m++) {
                        if (data_bck[m] == info[i].code)
                            break;
                    }
                    if (m == bck_num && bck_num < 6) {
                        data_bck[bck_num] = info[i].code;
                        bck_num++;
                    }
                    break;
                }
            }
            if (old_data)
                break;
            for (m = 0; m < new_num; m++) {
                if (data_new[m] == info[i].code)
                    break;
            }
            if (m == new_num && new_num < 6) {
                data_new[new_num] = info[i].code;
                new_num++;
            }
            break;
        }
    }

    memset(&hid_data[2], 0, 6);
    if (bck_num > 0)
        memcpy(&hid_data[2], data_bck, bck_num > 6 ? 6 : bck_num);
    if (new_num > 0 && bck_num < 6)
        memcpy(&hid_data[2 + bck_num], data_new, 6 - bck_num);
}

static ssize_t nc60_v2_key_hid_write(const void *buffer, size_t size)
{
    return g_hid_dev->ops.write(g_hid_dev, 1, buffer, size);
}

static void nc60_v2_key_task_entry(void* parameter)
{
    uint8_t hid_data[8] = { 0 };
    uint8_t hid_data_old[8] = { 0 };
    uint32_t key_raw_data[KEY_RAW_DATA_NUM] = { 0 };
    uint32_t key_raw_data_old[KEY_RAW_DATA_NUM] = { 0 };
    bool update = false;
    int i = 0;

    memset(def_key_info, 0, sizeof(struct key_info) * KEY_NUM);

    g_hid_dev = NULL;
    for (i = 0; i < 100; i ++) {
        g_hid_dev = device_find_by_name("hidd");
        if (g_hid_dev == NULL) {
            i++;
            pr_err("%s device not found, retry=%d\r\n", "hidd", i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\r\n", "hidd");
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\r\n", "hidd");
        return;
    }

    while (1) {
        nk60_v2_key_scan(key_raw_data);
        for (i = 0; i < KEY_RAW_DATA_NUM; i++) {
            if (key_raw_data[i] != key_raw_data_old[i]) {
                key_raw_data_old[i] = key_raw_data[i];
                update = true;
            }
        }
        if (update) {
            update = false;
            nk60_v2_key_info_update(key_raw_data, def_key_info);
            // key_action(def_key_info);
            nk60_v2_hid_data_update(def_key_info, hid_data);
            for (i = 0; i < 8; i++) {
                if (hid_data[i] != hid_data_old[i]) {
                    hid_data_old[i] = hid_data[i];
                    update = true;
                }
            }
            if (update) {
                update = false;
                nc60_v2_key_hid_write(hid_data, 8);
            }
        }
        msleep(10);
    }
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
    nk60_v2_key_gpio_init();

    key_task = task_create("nc60_v2-key", nc60_v2_key_task_entry, NULL, 3, 1024, 10, NULL);
    if (key_task == NULL) {
        pr_fatal("creat nc60_v2-key task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(key_task);

    return 0;
}
task_init(nk60_v2_key_init);