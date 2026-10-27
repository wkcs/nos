/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V2 Keyboard Profile Configuration:
 * - 74HC595 hardware scan-bit to logical key index mapping (position_index)
 * - Default 3-layer keycode layout (def_key_code_layout) & action map (def_action_layout)
 * - Registered with the generic Keyboard Server at application init time
 */

#include <kernel/init.h>
#include <keyboard/keyboard.h>

#define NK60_KEY_NUM        61U
#define NK60_KEY_LAYER_NUM  3U
#define NK60_RAW_BIT_NUM    (8U * 9U)

static const int8_t g_nk60_position_index[NK60_RAW_BIT_NUM] = {
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

static const uint8_t g_nk60_def_key_code_layout[NK60_KEY_LAYER_NUM][NK60_KEY_NUM] = {
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

static const uint8_t g_nk60_def_action_layout[NK60_KEY_LAYER_NUM][NK60_KEY_NUM] = {
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

static const struct keyboard_profile g_nk60_kb_profile = {
    .name               = "NK60-V2",
    .rows               = 5U,
    .cols               = 14U,
    .key_count          = NK60_KEY_NUM,
    .layer_count        = NK60_KEY_LAYER_NUM,
    .form_factor        = NLK_KB_FORM_60,
    .switch_type        = NLK_KB_SW_MECH,
    .kb_flags           = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO |
                          NLK_KB_FLAG_MEDIA_KEYS | NLK_KB_FLAG_MODULAR_HUB |
                          NLK_KB_FLAG_LIGHT_RGB_KEY | NLK_KB_FLAG_LIGHT_UNDERGLOW |
                          NLK_KB_FLAG_LIGHT_INDICATORS,
    .debounce_ms        = 10U,
    .led_matrix_rows    = 5U,
    .led_matrix_cols    = 14U,
    .underglow_leds     = 9U,
    .led_count          = 70U,
    .poll_rate_hz       = 100U,
    .raw_bit_num        = NK60_RAW_BIT_NUM,
    .position_index     = g_nk60_position_index,
    .default_keymap     = &g_nk60_def_key_code_layout[0][0],
    .default_action_map = &g_nk60_def_action_layout[0][0],
};

static int nk60_keyboard_profile_init(void)
{
    keyboard_server_register_profile(&g_nk60_kb_profile);
    return 0;
}
SYS_INIT(nk60_keyboard_profile_init, INIT_LEVEL_APPLICATION);
