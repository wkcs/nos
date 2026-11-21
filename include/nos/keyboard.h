/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Keyboard Hardware Driver & Keyboard Server Public Interface.
 */

#ifndef __NOS_KEYBOARD_H__
#define __NOS_KEYBOARD_H__

#include <stdint.h>
#include <stdbool.h>
#include <uapi/nos/kb_keycodes.h>
#include <uapi/nos/nlk.h>
#include <nos/keyboard_engine.h>

#define KEYMAP( \
    K00, K01, K02, K03, K04, K05, K06, K07, K08, K09, K0A, K0B, K0C, K0D, \
    K10, K11, K12, K13, K14, K15, K16, K17, K18, K19, K1A, K1B, K1C, K1D, \
    K20, K21, K22, K23, K24, K25, K26, K27, K28, K29, K2A, K2B,      K2D, \
    K30, K31, K32, K33, K34, K35, K36, K37, K38, K39, K3A,           K3D, \
    K40, K41, K42,                K46,                K4A, K4B, K4C, K4D  \
) { \
    KC_##K00, KC_##K01, KC_##K02, KC_##K03, KC_##K04, KC_##K05, KC_##K06, KC_##K07, KC_##K08, KC_##K09, KC_##K0A, KC_##K0B, KC_##K0C, KC_##K0D, \
    KC_##K10, KC_##K11, KC_##K12, KC_##K13, KC_##K14, KC_##K15, KC_##K16, KC_##K17, KC_##K18, KC_##K19, KC_##K1A, KC_##K1B, KC_##K1C, KC_##K1D, \
    KC_##K20, KC_##K21, KC_##K22, KC_##K23, KC_##K24, KC_##K25, KC_##K26, KC_##K27, KC_##K28, KC_##K29, KC_##K2A, KC_##K2B,           KC_##K2D, \
    KC_##K30, KC_##K31, KC_##K32, KC_##K33, KC_##K34, KC_##K35, KC_##K36, KC_##K37, KC_##K38, KC_##K39, KC_##K3A,                     KC_##K3D, \
    KC_##K40, KC_##K41, KC_##K42,                               KC_##K46,                               KC_##K4A, KC_##K4B, KC_##K4C, KC_##K4D  \
}

/* Default Key Action Binding Entry (by action name string) */
struct key_action_default_binding {
    uint8_t     layer;        /* Target layer (0..NLK_KB_MAX_LAYERS-1) */
    uint8_t     key_index;    /* Key matrix index (0..key_count-1) */
    const char *action_name;  /* Action name string matching registered capability */
};

/* Hardware Key Matrix Driver Control Commands & Descriptor */
#ifndef KEY_MAX_LAYERS
#define KEY_MAX_LAYERS              8U
#endif
#define KEY_MAX_NUM                 128U
#define KEY_MAX_RAW_BITS            128U
#define KEY_RAW_WORDS(key_num)      (((key_num) + 31U) / 32U)
#define KEY_RAW_GROUP(index)        ((index) / 32U)
#define KEY_RAW_BIT(index)          ((index) % 32U)

enum key_ctrl_cmd {
    KEY_CTRL_ENABLE = 0,
    KEY_CTRL_DISABLE,
    KEY_CTRL_GET_ENABLE_STATUS,
    KEY_CTRL_GET_DEV_INFO,
    KEY_CTRL_SET_DEBOUNCE,
    KEY_CTRL_SET_POLL_RATE,
    KEY_CTRL_GET_STATS,
};

struct key_dev_info {
    uint16_t key_count;                /* Number of logical keys returned in key_bitmap by read() */
    uint16_t debounce_ms;              /* Current hardware/filter debounce time */
    uint16_t poll_rate_hz;             /* Scan rate */
};

struct key_driver_stats {
    uint32_t scan_count;               /* Total scans completed */
    uint32_t key_down_events;          /* Total key down events detected */
    uint32_t error_count;              /* Hardware/communication errors */
};

/* Registered Layout Definition in Keyboard Profile */
struct keyboard_profile_layout {
    struct nlk_kb_layout_info info;
    const uint8_t            *default_keymap;     /* Flattened [layer_count][key_count] */
};

struct keyboard_light_effect {
    const char *name;
    const char *desc;
    nlk_kb_effect_render_fn_t render;
};

struct keyboard_light_layer_default {
    const char *action; /* Optional action name controlling this ordinary effect. */
    struct nlk_kb_light_layer layer;
};

/* Keyboard Profile (Default Keymap/Action Layout managed by Keyboard Server) */
struct keyboard_profile {
    const char                               *name;
    const char                               *hardware_id;       /* Stable model/revision/layout ID */
    const char *storage_device;           /* Device expected at the configuration mount. */
    uint8_t                                   rows;
    uint8_t                                   cols;
    uint8_t                                   key_count;
    uint8_t                                   layer_count;
    uint8_t                                   form_factor;       /* enum nlk_kb_form_factor */
    uint8_t                                   switch_type;       /* enum nlk_kb_switch_type */
    uint16_t                                  kb_flags;          /* NLK_KB_FLAG_* */
    uint8_t                                   debounce_ms;
    uint8_t                                   led_matrix_rows;
    uint8_t                                   led_matrix_cols;
    uint8_t                                   underglow_leds;
    uint16_t                                  led_count;
    uint16_t                                  poll_rate_hz;
    const uint8_t                            *default_keymap;     /* Flattened [layer_count][key_count] */

    /* Default Action Bindings by Capability Name */
    uint8_t                                   default_action_binding_count;
    const struct key_action_default_binding  *default_action_bindings;

    /* Multi-Layout Registration */
    uint8_t                               layout_count;
    const struct keyboard_profile_layout *layouts;

    /* Keep registration order stable: profiles persist these device-local IDs. */
    uint8_t                               light_effect_count;
    const struct keyboard_light_effect   *light_effects;

    /* Multi-Zone Backlight Registration */
    uint8_t                               light_zone_count;
    const struct nlk_kb_light_zone_desc  *light_zones;
    const struct nlk_kb_zone_light_cfg   *default_zone_cfgs;

    /* Factory configuration, replaced by saved lighting configurations at runtime. */
    uint8_t light_layer_count;
    const struct keyboard_light_layer_default *light_layers;

    /* Dynamic Action Registration */
    uint8_t                               action_count;
    const struct nlk_kb_action_desc      *action_descs;
    const key_action_handler_fn_t        *action_handlers;

};

/* Keyboard Server Control Commands */
enum ks_ctrl_cmd {
    KS_CTRL_ENABLE = 0,
    KS_CTRL_DISABLE,
    KS_CTRL_GET_ENABLE_STATUS,
    KS_CTRL_GET_DEV_INFO,
    KS_CTRL_GET_ACTIVE_LAYER,
    KS_CTRL_GET_PARAMS,
    KS_CTRL_SET_PARAMS,
    KS_CTRL_GET_ACTIVE_LAYOUT,
    KS_CTRL_SET_ACTIVE_LAYOUT,
    KS_CTRL_SAVE_PROFILE,
    KS_CTRL_LOAD_PROFILE,
    KS_CTRL_SWITCH_PROFILE,
    KS_CTRL_GET_PROFILE_INFO,
    KS_CTRL_GET_LIGHT_CFG,
    KS_CTRL_GET_INDICATORS,
};

struct kb_effect_definition;
struct keyboard_light_layer_state {
    struct nlk_kb_light_layer layer;
    nlk_kb_effect_render_fn_t render;
    const struct kb_effect_definition *definition;
    struct kb_effect_parameters parameters;
    /* 触发该灯效图层的动作上下文与参数 */
    struct kb_light_trigger_data trigger;
    void *priv;
    bool active;
};

struct keyboard_light_state {
    struct keyboard_light_layer_state layers[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    uint32_t generation;
    uint8_t count;
    bool enabled;
};

void keyboard_server_get_light_state(struct keyboard_light_state *state);

void keyboard_server_register_profile(const struct keyboard_profile *profile);

#endif /* __NOS_KEYBOARD_H__ */
