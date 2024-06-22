/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/task.h>
#include <kernel/err.h>
#include <kernel/sleep.h>
#include <init/init.h>
#include <string.h>
#include <gpio.h>
#include <board.h>
#include <cmd/cmd.h>
#include "keyboard.h"

#define KEY_NUM 61
#define KEY_RAW_DATA_NUM ((KEY_NUM) / 32 + (((KEY_NUM) % 32) ? 1 : 0))
#define KEY_RAW_DATA_GROUP(index) ((index) / 32)
#define KEY_RAW_DATA_INDEX(index) ((index) % 32)

#define KEY_X1 PCout(14)
#define KEY_X2 PAout(4)
#define KEY_X3 PAout(8)
#define KEY_X4 PAout(15)
#define KEY_X5 PBout(14)

#define KEY_Y1  PCin(0)
#define KEY_Y2  PCin(1)
#define KEY_Y3  PCin(2)
#define KEY_Y4  PCin(3)
#define KEY_Y5  PCin(4)
#define KEY_Y6  PCin(5)
#define KEY_Y7  PCin(6)
#define KEY_Y8  PCin(7)
#define KEY_Y9  PCin(8)
#define KEY_Y10 PCin(9)
#define KEY_Y11 PCin(10)
#define KEY_Y12 PCin(11)
#define KEY_Y13 PCin(12)
#define KEY_Y14 PCin(13)

#define KEY_TASK_PRIO       6
#define KEY_TASK_STACK_SIZE 4096
#define KEY_TASK_TICK       3

#define KEY_UP   0
#define KEY_DOWN 1

#define DEF_LAYER 0
#define FN_LAYER  1
#define PN_LAYER  2

static struct task_struct_t *key_task;

extern void hid_write_test(const void *buffer, size_t size);

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

static int position_index[5][14] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13},
    {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},
    {28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, -1, 40},
    {41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, 52},
    {53, 54, 55, -1, -1, 56, -1, -1, -1, 57, 58, 59, -1 ,60},
};

static inline void key_set_x_out(uint8_t x, int val)
{
    switch(x) {
        case 0: KEY_X1 = val; break;
        case 1: KEY_X2 = val; break;
        case 2: KEY_X3 = val; break;
        case 3: KEY_X4 = val; break;
        case 4: KEY_X5 = val; break;
        default:
            pr_err("no this key line(%d)\r\n", x);
            break;
    }

    return;
}

static inline int key_get_y_val(uint8_t y)
{
    switch(y) {
        case 0: return KEY_Y1;
        case 1: return KEY_Y2;
        case 2: return KEY_Y3;
        case 3: return KEY_Y4;
        case 4: return KEY_Y5;
        case 5: return KEY_Y6;
        case 6: return KEY_Y7;
        case 7: return KEY_Y8;
        case 8: return KEY_Y9;
        case 9: return KEY_Y10;
        case 10: return KEY_Y11;
        case 11: return KEY_Y12;
        case 12: return KEY_Y13;
        case 13: return KEY_Y14;
        default:
            pr_err("no this key line(%d)\r\n", y);
            return -1;
    }
}

static void key_scan(uint32_t *raw_data)
{
    uint32_t raw_data_old[KEY_RAW_DATA_NUM] = { 0 };
    int x, y;
    int index;

    memset(raw_data, 0, KEY_RAW_DATA_NUM * sizeof(uint32_t));

    for (x = 0; x < 5; x++) {
        key_set_x_out(x, KEY_DOWN);
        for (y = 0; y < 14; y ++) {
            index = position_index[x][y];
            if (index < 0)
                continue;
            if (key_get_y_val(y) == KEY_DOWN)
                raw_data_old[KEY_RAW_DATA_GROUP(index)] |= 1 << KEY_RAW_DATA_INDEX(index);
        }
        key_set_x_out(x, KEY_UP);
    }
    delay_msec(10);
    for (x = 0; x < 5; x++) {
        key_set_x_out(x, KEY_DOWN);
        for (y = 0; y < 14; y ++) {
            index = position_index[x][y];
            if (index < 0)
                continue;
            if (key_get_y_val(y) == KEY_DOWN)
                raw_data[KEY_RAW_DATA_GROUP(index)] |= 1 << KEY_RAW_DATA_INDEX(index);
        }
        key_set_x_out(x, KEY_UP);
    }

    for (index = 0; index < KEY_RAW_DATA_NUM; index++)
        raw_data[index] &= raw_data_old[index];
}

static int get_key_layer(uint32_t *raw_data, struct key_info *old_info, int *special_key_layout)
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

static void key_info_update(uint32_t *raw_data, struct key_info *info)
{
    int i;
    int layer;

    layer = get_key_layer(raw_data, info, def_key_code_layout[DEF_LAYER]);
    for (i = 0; i < 64; i++) {
        if ((raw_data[KEY_RAW_DATA_GROUP(i)] >> KEY_RAW_DATA_INDEX(i)) & 0x01) {
            if (info[i].val != KEY_DOWN) {
                info[i].code = def_key_code_layout[layer][i];
                info[i].val = KEY_DOWN;
                info[i].action = def_action_layout[layer][i];
                //pr_info("key down: layer=%d, index=%d, code=0x%02x, action=%d\r\n", layer, i, info[i].code, info[i].action);
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

static void key_hid_data_update(struct key_info *info, uint8_t *hid_data)
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

static void key_action(struct key_info *info)
{
    int i;

    for (i = 0; i < 64; i++) {
        if (info[i].val != KEY_DOWN)
            continue;
        if (info[i].action == KA_NO)
            continue;

        key_action_run(info[i].action);
    }
}

static void key_task_entry(__maybe_unused void* parameter)
{
    uint8_t hid_data[8] = { 0 };
    uint8_t hid_data_old[8] = { 0 };
    uint32_t key_raw_data[KEY_RAW_DATA_NUM] = { 0 };
    uint32_t key_raw_data_old[KEY_RAW_DATA_NUM] = { 0 };
    bool update = false;
    int i;

    memset(def_key_info, 0, sizeof(struct key_info) * KEY_NUM);

    while (1) {
        key_scan(key_raw_data);
        for (i = 0; i < KEY_RAW_DATA_NUM; i++) {
            if (key_raw_data[i] != key_raw_data_old[i]) {
                key_raw_data_old[i] = key_raw_data[i];
                update = true;
            }
        }
        if (update) {
            update = false;
            key_info_update(key_raw_data, def_key_info);
            key_action(def_key_info);
            key_hid_data_update(def_key_info, hid_data);
            for (i = 0; i < 8; i++) {
                if (hid_data[i] != hid_data_old[i]) {
                    hid_data_old[i] = hid_data[i];
                    update = true;
                }
            }
            if (update) {
                update = false;
                hid_write_test(hid_data, 8);
            }
        }
        delay_msec(20);
    }
}

int keyboard_init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_8 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOA, GPIO_Pin_4 | GPIO_Pin_8 | GPIO_Pin_15);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOC, GPIO_Pin_14);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 |
        GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7 |
        GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12 |
        GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    key_task = task_create("key", key_task_entry, NULL, KEY_TASK_STACK_SIZE, KEY_TASK_PRIO,
        1024, KEY_TASK_TICK, NULL, NULL);
    if (key_task == NULL) {
        pr_err("creat key task err\n");
        return -1;
    }
    task_ready(key_task);

    return 0;
}
task_init(keyboard_init);
