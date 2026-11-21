/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Keyboard engine and its NLK capability adapter.
 */
#ifndef NOS_KEYBOARD_ENGINE_H
#define NOS_KEYBOARD_ENGINE_H

#include <nos/nlk_core.h>
#include <uapi/nos/kb_keycodes.h>

#define NLK_KB_MAX_LAYERS 8U
#define NLK_KB_MAX_KEYS 128U

#ifndef CONFIG_KEYBOARD_LIGHT_MAX_LAYERS
#define CONFIG_KEYBOARD_LIGHT_MAX_LAYERS 8
#endif

typedef void (*nlk_kb_effect_render_fn_t)(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                                          uint32_t phase, uint8_t rgb[3], void *priv);
struct nlk_kb_effect_entry {
    /* Registered definition names live in read-only storage for this lifetime. */
    const char *name;
    uint8_t effect_id;
    nlk_kb_effect_render_fn_t render;
    void *priv;
    struct nlk_kb_effect_entry *next;
};

static inline const char *nlk_kb_effect_name(const struct nlk_kb_effect_entry *entry)
{
    if (entry == NULL) {
        return "";
    }
    return entry->name;
}

static inline uint8_t nlk_kb_effect_id(const struct nlk_kb_effect_entry *entry)
{
    return entry->effect_id;
}

typedef void (*nlk_kb_report_cb_t)(const struct nlk_kb_hid_report *report, void *priv);
typedef void (*nlk_kb_nkro_cb_t)(const struct nlk_kb_nkro_report *report, void *priv);
typedef void (*nlk_kb_light_cb_t)(const struct nlk_kb_light_cfg *cfg,
                                  const struct nlk_kb_light_sync_beacon *beacon, uint8_t indicators,
                                  void *priv);
typedef void (*nlk_kb_zone_light_cb_t)(uint8_t zone_id, const struct nlk_kb_zone_light_cfg *cfg,
                                       void *priv);
typedef void (*nlk_kb_light_frame_cb_t)(uint16_t start_led, uint16_t led_count,
                                        const uint8_t *rgb_data, uint16_t len, void *priv);
typedef int (*nlk_kb_param_change_cb_t)(const struct nlk_kb_params *params, void *priv);
/* A keyboard configuration worker attaches to the existing keyboard capability. */
struct nlk_kb_config_ops {
    int (*rpc)(struct nlk_node *node, const struct nlk_pkt_hdr *header, const void *request,
               uint16_t length, void *response, uint16_t *response_length, void *priv);
    void (*poll)(void *priv, uint32_t now_ms);
};

struct nlk_kb_macro {
    struct nlk_kb_macro *next;
    struct nlk_kb_macro_info info;
    char desc[NLK_KB_MACRO_DESC_MAX];
    struct nlk_kb_macro_step steps[];
};

#define KB_EFFECT_PARAMETERS_MAX 64U
struct kb_effect_parameters {
    uint8_t length;
    uint8_t data[KB_EFFECT_PARAMETERS_MAX];
};

#define KB_LIGHT_TRIGGER_ARGS_MAX 32U
#define KB_LIGHT_COORD_NONE       0xFFU

/* 动作触发灯效的事件类型 */
enum kb_light_trigger_event {
    KB_LIGHT_TRIG_NONE = 0U,
    KB_LIGHT_TRIG_KEY_UP = 1U,
    KB_LIGHT_TRIG_KEY_DOWN = 2U,
    KB_LIGHT_TRIG_STATE_SYNC = 3U,
};

/* 动作向引擎反馈的逻辑状态与附加数值 */
struct kb_action_state {
    bool active;
    uint32_t value;
};

/* 动作传递给灯效图层的通用触发上下文与参数 */
struct kb_light_trigger_data {
    uint32_t seq;
    uint32_t key_code;
    uint32_t value;
    uint8_t event;
    uint8_t active;
    uint8_t layer;
    uint8_t key_index;
    uint8_t key_x;
    uint8_t key_y;
    uint8_t arg_length;
    uint8_t arguments[KB_LIGHT_TRIGGER_ARGS_MAX];
};

struct nlk_keyboard_ctx;

typedef bool (*nlk_kb_action_state_fn_t)(const struct nlk_keyboard_ctx *ctx, uint32_t trigger,
                                         struct kb_action_state *out, void *priv);

/* 独立分配的每层运行时数据，避免撑大 nlk_keyboard_ctx 静态上下文体积 */
struct kb_light_layer_runtime {
    struct kb_effect_parameters parameters[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    struct kb_light_trigger_data triggers[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
};

struct nlk_keyboard_ctx {
    struct nlk_kb_desc desc;
    uint8_t active_layer;
    uint8_t default_layer;
    uint8_t indicator_leds;
    uint8_t module_key_offset[8];
    uint32_t *keymap[NLK_KB_MAX_LAYERS];
    struct nlk_kb_key_action_chain *action_chains[NLK_KB_MAX_LAYERS];
    uint8_t *key_state;
    uint32_t *active_code;
    uint8_t key_capacity;
    uint8_t layer_capacity;
    struct nlk_kb_hid_report last_report;
    struct nlk_kb_nkro_report last_nkro;
    uint16_t consumer_report;
    struct nlk_kb_analog_cfg analog_cfg;
    struct nlk_kb_light_cfg light_cfg;
    struct kb_light_layer_runtime *light_runtime;
    uint8_t light_layer_count;
    struct nlk_kb_light_layer light_layers[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    bool light_layer_active[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    uint32_t light_trigger_seq;
    uint32_t light_layer_generation;
    struct nlk_kb_light_sync_beacon last_light_beacon;
    uint32_t next_beacon_ms;

    /* Multi-layout management */
    uint8_t layout_count;
    uint8_t active_layout_id;

    /* Device-local lighting effects */
    uint16_t light_effect_count;
    struct nlk_kb_effect_entry *light_effects;
    struct nlk_kb_effect_entry *light_effects_tail;

    /* Multi-zone lighting */
    uint8_t light_zone_count;
    struct nlk_kb_light_zone_desc light_zones[NLK_KB_MAX_LIGHT_ZONES];
    struct nlk_kb_zone_light_cfg zone_cfgs[NLK_KB_MAX_LIGHT_ZONES];
    nlk_kb_zone_light_cb_t zone_light_cb;
    void *zone_light_priv;

    /* Macro playback */
    uint32_t macro_clock_ms;
    uint32_t macro_due_ms;
    uint8_t macro_playing;
    uint8_t macro_play_id;
    uint8_t macro_play_index;
    uint8_t macro_play_count;
    uint8_t macro_repeats_left;
    uint8_t macro_tap_pending;
    uint8_t macro_modifiers;
    uint8_t macro_keys[6];
    struct nlk_kb_macro_step macro_play_steps[NLK_KB_MAX_MACRO_STEPS];
    struct nlk_kb_macro *macros;
    uint8_t macro_count;

    /* Dynamic Action Bindings */
    /* Names reference immutable definitions that outlive this context. */
    const char *action_names[NLK_KB_MAX_ACTIONS];
    uint8_t action_count;
    key_action_handler_fn_t action_handlers[NLK_KB_MAX_ACTIONS];
    void *action_handler_privs[NLK_KB_MAX_ACTIONS];
    nlk_kb_action_state_fn_t action_state_cb;
    void *action_state_priv;
    bool action_trigger_published;

    /* Keyboard runtime parameters */
    struct nlk_kb_params params;
    nlk_kb_param_change_cb_t param_cb;
    void *param_priv;

    nlk_kb_report_cb_t report_cb;
    nlk_kb_nkro_cb_t nkro_cb;
    void *report_priv;
    nlk_kb_light_cb_t light_cb;
    nlk_kb_light_frame_cb_t light_frame_cb;
    void *light_priv;
    struct nlk_node *node;
    const struct nlk_kb_config_ops *config_ops;
    void *config_priv;
    struct nlk_capability cap;
};

/* Registry order defines production action IDs; zero is never a valid ID. */
static inline uint8_t nlk_cap_keyboard_action_id(const struct nlk_keyboard_ctx *ctx, uint8_t index)
{
    if (ctx == NULL || index >= ctx->action_count || index >= NLK_KB_MAX_ACTIONS) {
        return 0U;
    }
    return (uint8_t)(index + 1U);
}

int nlk_cap_keyboard_init(struct nlk_node *node, struct nlk_keyboard_ctx *ctx, uint8_t rows,
                          uint8_t cols, uint8_t key_count, nlk_kb_report_cb_t report_cb,
                          void *priv);
int nlk_cap_keyboard_init_sized(struct nlk_node *node, struct nlk_keyboard_ctx *ctx, uint8_t rows,
                                uint8_t cols, uint8_t key_count, uint8_t layer_count,
                                nlk_kb_report_cb_t report_cb, void *priv);
/* Only for an unregistered context, or after its node is no longer in use. */
int nlk_cap_keyboard_attach(struct nlk_keyboard_ctx *ctx, struct nlk_node *node);
void nlk_cap_keyboard_deinit(struct nlk_keyboard_ctx *ctx);
int nlk_cap_keyboard_configure(struct nlk_keyboard_ctx *ctx, const struct nlk_kb_desc *desc);
int nlk_cap_keyboard_set_light_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_light_cb_t light_cb,
                                  nlk_kb_light_frame_cb_t frame_cb, void *priv);
int nlk_cap_keyboard_set_zone_light_cb(struct nlk_keyboard_ctx *ctx,
                                       nlk_kb_zone_light_cb_t zone_light_cb, void *priv);
int nlk_cap_keyboard_set_param_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_param_change_cb_t param_cb,
                                  void *priv);
int nlk_cap_keyboard_register_layout(struct nlk_keyboard_ctx *ctx,
                                     const struct nlk_kb_layout_info *layout);
const struct nlk_kb_effect_entry *
nlk_cap_keyboard_find_light_effect(const struct nlk_keyboard_ctx *ctx, uint8_t effect_id);
/* Entries live as long as the context; production names must also live that long. */
int nlk_cap_keyboard_register_light_effect(struct nlk_keyboard_ctx *ctx, const char *name,
                                           const char *desc, nlk_kb_effect_render_fn_t handler,
                                           void *priv);
int nlk_cap_keyboard_register_light_zone(struct nlk_keyboard_ctx *ctx,
                                         const struct nlk_kb_light_zone_desc *zone,
                                         const struct nlk_kb_zone_light_cfg *initial_cfg);
int nlk_cap_keyboard_validate_light_layer(const struct nlk_keyboard_ctx *ctx,
                                          const struct nlk_kb_light_layer *layer);
int nlk_cap_keyboard_set_light_layers(struct nlk_keyboard_ctx *ctx,
                                      const struct nlk_kb_light_layer *layers, uint8_t count);
void nlk_cap_keyboard_set_action_state_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_action_state_fn_t cb,
                                          void *priv);
void nlk_cap_keyboard_notify_light_trigger(struct nlk_keyboard_ctx *ctx, const char *action,
                                           const struct kb_light_trigger_data *trigger);
void nlk_cap_keyboard_sync_action_states(struct nlk_keyboard_ctx *ctx);
void nlk_cap_keyboard_set_indicators(struct nlk_keyboard_ctx *ctx, uint8_t indicator_leds);
/* Production names must remain valid and immutable for the context lifetime. */
int nlk_cap_keyboard_register_action(struct nlk_keyboard_ctx *ctx, const char *name,
                                     const char *desc, key_action_handler_fn_t handler, void *priv);
int nlk_cap_keyboard_chord_action(const struct key_action_context *ctx, void *priv);
int nlk_cap_keyboard_bind_key_action(struct nlk_keyboard_ctx *ctx, uint8_t layer, uint8_t key_index,
                                     const char *action_name, bool append);
int nlk_cap_keyboard_unbind_key_action(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                       uint8_t key_index, const char *action_name);
int nlk_cap_keyboard_bind_all_keys(struct nlk_keyboard_ctx *ctx, uint8_t layer_mask,
                                   const char *action_name, bool append);
struct nlk_kb_macro *nlk_cap_keyboard_find_macro(const struct nlk_keyboard_ctx *ctx, uint8_t id);
struct nlk_kb_macro *nlk_cap_keyboard_alloc_macro(uint8_t capacity);
void nlk_cap_keyboard_free_macros(struct nlk_kb_macro *macro);
uint8_t nlk_cap_keyboard_next_macro_id(const struct nlk_keyboard_ctx *ctx);
void nlk_cap_keyboard_replace_macros(struct nlk_keyboard_ctx *ctx, struct nlk_kb_macro *macros,
                                     uint8_t count);
int nlk_cap_keyboard_set_macro(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, const char *name,
                               const struct nlk_kb_macro_step *steps, uint8_t step_count,
                               uint8_t repeat_count);
int nlk_cap_keyboard_set_macro_meta(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, const char *id,
                                    const char *name, const char *desc,
                                    const struct nlk_kb_macro_step *steps, uint8_t step_count,
                                    uint8_t repeat_count);
int nlk_cap_keyboard_macro_start(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, uint32_t now_ms);
int nlk_cap_keyboard_macro_cancel(struct nlk_keyboard_ctx *ctx);
int nlk_cap_keyboard_tick(struct nlk_keyboard_ctx *ctx, uint32_t now_ms);
int nlk_cap_keyboard_set_params(struct nlk_keyboard_ctx *ctx, const struct nlk_kb_params *params);
int nlk_cap_keyboard_set_module_offset(struct nlk_keyboard_ctx *ctx, uint8_t module_id,
                                       uint8_t key_offset);
int nlk_cap_keyboard_load_layout(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                 const uint8_t *key_codes, uint8_t count);
int nlk_cap_keyboard_load_layout32(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                   const uint32_t *key_codes, uint8_t count);
int nlk_cap_keyboard_update_local_keys(struct nlk_keyboard_ctx *ctx, const uint32_t *raw_bitmap,
                                       uint8_t key_count);
void nlk_cap_keyboard_release_held_keys(struct nlk_keyboard_ctx *ctx);
int nlk_cap_keyboard_report_switches(struct nlk_keyboard_ctx *ctx, nlk_addr_t target_addr,
                                     uint8_t dst_cap, uint8_t module_id,
                                     const struct nlk_kb_switch_item *items, uint8_t count);
int nlk_cap_keyboard_broadcast_light_beacon(struct nlk_keyboard_ctx *ctx,
                                            uint32_t master_frame_tick);

#endif
