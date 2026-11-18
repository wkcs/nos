/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_KEYBOARD_DEFINITIONS_H
#define NOS_KEYBOARD_DEFINITIONS_H
#include <stddef.h>
#include <nos/keyboard.h>

#define KB_EFFECT_FIELD_BRIGHTNESS 1U
#define KB_EFFECT_FIELD_SPEED 2U
#define KB_EFFECT_FIELD_COLOR 4U
#define KB_EFFECT_ENGINE_LIMIT 16U
enum kb_parameter_type {
    KB_PARAMETER_U8 = 1,
    KB_PARAMETER_RGB,
    KB_PARAMETER_BASE_REF,
    KB_PARAMETER_MACRO_REF,
};
struct kb_parameter_definition {
    uint16_t id;
    uint8_t type;
    uint16_t minimum, maximum, default_value;
    const char *name;
};
/* Keyboard services, independent of NLK RUN opcodes and action registration. */
enum kb_action_request {
    KB_ACTION_REQUEST_SELECT_BASE,
    KB_ACTION_REQUEST_BASE_NEXT,
    KB_ACTION_REQUEST_BASE_PREV,
    KB_ACTION_REQUEST_LIGHT_NEXT,
    KB_ACTION_REQUEST_LIGHT_PREV,
    KB_ACTION_REQUEST_LIGHT_TOGGLE,
    KB_ACTION_REQUEST_MACRO_PLAY,
    KB_ACTION_REQUEST_MACRO_STOP,
};
struct kb_action_runtime {
    void *context;
    /* Queue work; never perform filesystem I/O in the scanning task. */
    int (*request)(void *context, enum kb_action_request request, const char *target);
};
struct kb_action_definition {
    const char *id, *description;
    /* Arguments are encoded in descriptor order: u8, RGB, or length-prefixed ID.
     * The worker validates this schema and its references before publication. */
    const struct kb_parameter_definition *parameters;
    uint8_t parameter_count;
    /* Optional cross-field validation after generic schema validation. */
    int (*validate_arguments)(const void *arguments, uint16_t length);
    /* Required for every action. Runs for both press and release in the matrix
     * task; each action owns its event policy and HID pass/drop/transform result. */
    int (*execute)(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime);
};
/* Render state belongs to one layer instance and is reset on configuration change.
 * Parameters are a frame-local immutable copy, so preview replacement cannot free
 * memory being consumed by the display thread. */
typedef void (*kb_configured_render_fn)(const struct nlk_kb_light_cfg *cfg,
                                        const struct kb_effect_parameters *parameters, uint8_t x,
                                        uint8_t y, uint32_t phase, uint8_t rgb[3], void *state);
#define KB_EFFECT_STATE_MAX 32U
struct kb_effect_definition {
    const char *id, *description;
    uint16_t cycle_order;
    bool exclude_from_cycle;
    uint8_t engine_id;
    uint8_t fields; /* bit 0 brightness, bit 1 speed, bit 2 RGB color. */
    nlk_kb_effect_render_fn_t render;
    const struct kb_parameter_definition *parameters;
    uint8_t parameter_count;
    uint8_t state_size;
    kb_configured_render_fn render_configured;
};
uint8_t kb_effect_parameter_u8(const struct kb_effect_parameters *parameters, uint16_t id,
                               uint8_t fallback);
struct kb_key_geometry {
    uint16_t x, y, width, height;
    uint8_t row, col, led;
};
struct kb_layout_definition {
    const char *id;
    uint16_t version;
    uint8_t key_count, layer_count, rows, cols;
    const struct kb_key_geometry *keys;
    const uint8_t *default_keymap;
    const struct kb_parameter_definition *parameters;
    uint8_t parameter_count;
    /* Layout-owned base record tags 0x1000..0x1fff, e.g. Hall thresholds.
     * Validation must cover every condition needed by the infallible apply. */
    int (*validate_record)(uint16_t tag, const void *value, uint16_t length);
    void (*apply_record)(uint16_t tag, const void *value, uint16_t length);
};
extern const struct kb_layout_definition kb_selected_layout;
extern const struct kb_action_definition *const kb_action_definitions[];
extern const size_t kb_action_definition_count;
extern const struct kb_effect_definition *const kb_effect_definitions[];
extern const size_t kb_effect_definition_count;
/* Adapt immutable definitions to the legacy engine's device-local descriptors.
 * Call during board init, before registering the resulting profile. */
int kb_definitions_apply_profile(struct keyboard_profile *profile,
                                 struct keyboard_profile_layout *layout,
                                 struct keyboard_light_effect effects[KB_EFFECT_ENGINE_LIMIT]);
#endif
