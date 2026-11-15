/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V1 Keyboard Profile Configuration:
 * - Default 3-layer keycode layout (def_key_code_layout) & action map (def_action_layout)
 * - Multi-Layout, Multi-Zone Backlight, Actions & Macro definitions
 * - Registered with the generic Keyboard Server at application init time
 */

#include <kernel/init.h>
#include <nos/keyboard.h>
#include "nk60_light_effects.h"

#define NK60_KEY_NUM        61U
#define NK60_KEY_LAYER_NUM  3U

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

static const struct key_action_default_binding g_nk60_default_action_bindings[] = {
    { 0U, 28U, "caps_lock" },    /* Layer 0 Key 28: 普通动作，可绑定灯效 */
    { 2U, 47U, "light_toggle" }, /* Layer 2 Key 47: 背光开关动作 */
};

static const struct keyboard_profile_layout g_nk60_layouts[] = {
    {
        .info = {
            .layout_id   = NLK_KB_LAYOUT_STD_60_ANSI,
            .key_count   = NK60_KEY_NUM,
            .matrix_rows = 5U,
            .matrix_cols = 14U,
            .name        = "ANSI-61",
        },
        .default_keymap     = &g_nk60_def_key_code_layout[0][0],
    },
};

static const struct nlk_kb_light_zone_desc g_nk60_light_zones[] = {
    {
        .zone_id           = 0U,
        .type              = NLK_KB_LIGHT_TYPE_RGB_MATRIX,
        .led_start         = 0U,
        .led_count         = 61U,
        .supported_effects = NK60_EFFECT_MASK,
        .name              = "Key-RGB",
    },
    {
        /* Keep ID 1 for saved profiles. Caps Lock shares one LED with Key-RGB. */
        .zone_id           = 1U,
        .type              = NLK_KB_LIGHT_TYPE_INDICATOR,
        .led_start         = 28U,
        .led_count         = 1U,
        .supported_effects = NK60_EFFECT_MASK,
        .name              = "Caps Lock",
    },
};

static const struct nlk_kb_zone_light_cfg g_nk60_default_zone_cfgs[] = {
    {
        .zone_id    = 0U,
        .enable     = 1U,
        .effect_id  = NK60_EFFECT_RAINBOW,
        .brightness = 200U,
        .speed      = 128U,
        .color_r    = 255U,
        .color_g    = 128U,
        .color_b    = 0U,
    },
    {
        .zone_id    = 1U,
        .enable     = 1U,
        .effect_id  = NK60_EFFECT_STATIC,
        .brightness = 255U,
        .speed      = 128U,
        .color_r    = 255U,
        .color_g    = 255U,
        .color_b    = 255U,
    },
};

/* Factory lighting configuration. Runtime profiles can replace the entire list. */
static const struct keyboard_light_layer_default g_nk60_factory_light_layers[] = {
    {
        .layer = {
            .z_order = -2, .width = 14U, .height = 5U,
            .opacity = 255U, .zone_id = 0U,
        },
    },
    {
        .action = "caps_lock",
        .layer = {
            .z_order = -1, .y = 2U, .width = 1U, .height = 1U,
            .opacity = 255U, .zone_id = 1U, .activation = NLK_KB_LIGHT_TOGGLE,
        },
    },
};

static const struct nlk_kb_action_desc g_nk60_actions[6] = {
    {
        .action_id = 1U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "light_toggle",
        .desc      = "Toggle RGB backlight",
    },
    {
        .action_id = 2U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "caps_lock",
        .desc      = "Toggle Caps Lock",
    },
    {
        .action_id = 3U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "macro_play",
        .desc      = "Play keyboard macro",
    },
    {
        .action_id = 4U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "key_light",
        .desc      = "Per-key reactive",
    },
    {
        .action_id = 5U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "config_next",
        .desc      = "Switch next config",
    },
    {
        .action_id = 6U,
        .flags     = 0U,
        .reserved  = 0U,
        .name      = "win_lock",
        .desc      = "Lock Windows key",
    },
};

static const struct keyboard_profile g_nk60_kb_profile = {
    .name               = "NK60-V1",
    .hardware_id        = "NK60-V1-ANSI61-v1",
    .rows               = 5U,
    .cols               = 14U,
    .key_count          = NK60_KEY_NUM,
    .layer_count        = NK60_KEY_LAYER_NUM,
    .form_factor        = NLK_KB_FORM_60,
    .switch_type        = NLK_KB_SW_MECH,
    .kb_flags           = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO |
                          NLK_KB_FLAG_MODULAR_HUB |
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
                          NLK_KB_FLAG_MEDIA_KEYS |
#endif
                          NLK_KB_FLAG_LIGHT_RGB_KEY |
                          NLK_KB_FLAG_LIGHT_INDICATORS,
    .debounce_ms        = 10U,
    .led_matrix_rows    = 5U,
    .led_matrix_cols    = 14U,
    .underglow_leds     = 0U,
    .led_count          = 61U,
    .poll_rate_hz       = 100U,
    .default_keymap     = &g_nk60_def_key_code_layout[0][0],

    /* Default Action Bindings */
    .default_action_binding_count = (uint8_t)(sizeof(g_nk60_default_action_bindings) /
                                              sizeof(g_nk60_default_action_bindings[0])),
    .default_action_bindings      = g_nk60_default_action_bindings,

    /* Multi-Layout Registration */
    .layout_count       = 1U,
    .layouts            = g_nk60_layouts,

    .light_effect_count = (uint8_t)(sizeof(g_nk60_light_effects) / sizeof(g_nk60_light_effects[0])),
    .light_effects      = g_nk60_light_effects,

    /* Per-key RGB backlight and shared Caps Lock indicator */
    .light_zone_count   = sizeof(g_nk60_light_zones) / sizeof(g_nk60_light_zones[0]),
    .light_zones        = g_nk60_light_zones,
    .default_zone_cfgs  = g_nk60_default_zone_cfgs,

    .light_layer_count = sizeof(g_nk60_factory_light_layers) / sizeof(g_nk60_factory_light_layers[0]),
    .light_layers = g_nk60_factory_light_layers,

    /* Dynamic Action Registration */
    .action_count       = 6U,
    .action_descs       = g_nk60_actions,
    .action_handlers    = NULL,

};

static int nk60_keyboard_profile_init(void)
{
    keyboard_server_register_profile(&g_nk60_kb_profile);
    return 0;
}
SYS_INIT(nk60_keyboard_profile_init, INIT_LEVEL_APPLICATION);
