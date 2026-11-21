/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0100: Multi-Form-Factor Keyboard (60%/65%/75%/TKL/96%/100%/
 * Split/Ortho/Alice/Numpad/Macropad, Mechanical & Hall-Analog Switches,
 * 6KRO + NKRO, Cascaded Sub-Module Aggregation & Integrated Keyboard Lighting).
 */

#define pr_fmt(fmt) "[KB] " fmt

#include <nos/keyboard_engine.h>
#include <kernel/printk.h>
#include <kernel/mm.h>
#include <kernel/irq.h>
#include <uapi/nos/errno.h>
#include <string.h>

#define KB_KEY_SUPPRESSED UINT32_MAX
#define KB_KEY_DROPPED (UINT32_MAX - 1U)
#define KB_KEY_CHORD 0xAFU /* Synthesized action output, never a USB usage */

static uint8_t kb_infer_form_factor(uint8_t key_count)
{
    if (key_count <= 16U) {
        return NLK_KB_FORM_MACROPAD;
    }
    if (key_count <= 24U) {
        return NLK_KB_FORM_NUMPAD;
    }
    if (key_count <= 50U) {
        return NLK_KB_FORM_40;
    }
    if (key_count <= 64U) {
        return NLK_KB_FORM_60;
    }
    if (key_count <= 72U) {
        return NLK_KB_FORM_65;
    }
    if (key_count <= 85U) {
        return NLK_KB_FORM_75;
    }
    if (key_count <= 90U) {
        return NLK_KB_FORM_TKL_80;
    }
    if (key_count <= 100U) {
        return NLK_KB_FORM_96;
    }
    return NLK_KB_FORM_ANSI_100;
}

static uint8_t kb_resolve_active_layer(const struct nlk_keyboard_ctx *ctx)
{
    uint8_t layer = ctx->default_layer;
    uint8_t i;

    /* Check if FN / PN / MO3 layer shift key is held on base layer 0 */
    for (i = 0U; i < ctx->desc.key_count; i++) {
        if (ctx->key_state[i] != 0U && ctx->active_code[i] != KB_KEY_SUPPRESSED) {
            uint8_t base_kc = ctx->keymap[0][i];
            if (base_kc == KC_MO3) {
                layer = 3U;
                break;
            }
            if (base_kc == KC_FN) {
                layer = 1U;
                break;
            }
            if (base_kc == KC_PN && layer == 0U) {
                layer = 2U;
            }
        }
    }
    if (layer >= ctx->desc.layer_count) {
        layer = 0U;
    }
    return layer;
}

static bool kb_is_function_key(uint8_t code)
{
    return (code >= KC_MEDIA_NEXT && code <= KC_MEDIA_BRIGHTNESS_DOWN) ||
           code == KB_KEY_CHORD;
}

int nlk_cap_keyboard_chord_action(const struct key_action_context *ctx, void *priv)
{
    uint8_t key;
    uint32_t modifiers;

    (void)priv;
    if (ctx == NULL || ctx->event_type != 1U) {
        return KEY_ACTION_DROP;
    }
    key = (uint8_t)ctx->key_code;
    modifiers = ctx->initial_code >> 8;
    if (modifiers == 0U || modifiers > 0xFFU || key < 4U || key > 0x65U) {
        return KEY_ACTION_DROP;
    }
    return (int)((modifiers << 16) | ((uint32_t)key << 8) | KB_KEY_CHORD);
}

static void kb_rebuild_hid_report(struct nlk_keyboard_ctx *ctx)
{
    /* 标准 USB HID Consumer Page (0x0C) 16 位用途码映射表 */
    static const uint16_t consumer_usages[] = {
        0x00B5U, /* KC_MEDIA_NEXT: Scan Next Track */
        0x00B6U, /* KC_MEDIA_PREV: Scan Previous Track */
        0x00CDU, /* KC_MEDIA_PLAY_PAUSE: Play/Pause */
        0x00E2U, /* KC_MEDIA_MUTE: Mute */
        0x00E9U, /* KC_MEDIA_VOL_UP: Volume Increment */
        0x00EAU, /* KC_MEDIA_VOL_DOWN: Volume Decrement */
        0x006FU, /* KC_MEDIA_BRIGHTNESS_UP: Display Brightness Increment */
        0x0070U, /* KC_MEDIA_BRIGHTNESS_DOWN: Display Brightness Decrement */
    };
    uint8_t prev_keys[6];
    uint8_t data_bck[6] = {0};
    uint8_t data_new[6] = {0};
    uint8_t bck_num = 0U;
    uint8_t new_num = 0U;
    uint8_t layer;
    uint8_t i, n, m;

    memcpy(prev_keys, ctx->last_report.keys, sizeof(prev_keys));
    layer = kb_resolve_active_layer(ctx);
    ctx->active_layer = layer;

    memset(&ctx->last_report, 0, sizeof(ctx->last_report));
    memset(&ctx->last_nkro, 0, sizeof(ctx->last_nkro));
    ctx->last_report.report_id = 1U;
    ctx->last_nkro.report_id = 2U;
    ctx->consumer_report = 0U;

    for (i = 0U; i < ctx->desc.key_count; i++) {
        uint8_t kc;
        uint8_t active_kc = (uint8_t)ctx->active_code[i];
        uint8_t mapped_kc = (uint8_t)ctx->keymap[layer][i];
        uint8_t modifiers = 0U;
        bool is_old;

        if (ctx->key_state[i] == 0U) {
            ctx->active_code[i] = 0U;
            continue;
        }
        if (ctx->active_code[i] == KB_KEY_SUPPRESSED || ctx->active_code[i] == KB_KEY_DROPPED) {
            continue;
        }
        /* Entering or leaving Fn must not change a held media key or shortcut. */
        if (kb_is_function_key(active_kc) || kb_is_function_key(mapped_kc) ||
            ctx->action_chains[layer][i].count > 0U) {
            kc = active_kc;
        } else if (layer > 0U && mapped_kc != 0U) {
            kc = mapped_kc;
            ctx->active_code[i] = kc;
        } else if (ctx->active_code[i] != 0U) {
            kc = ctx->active_code[i];
        } else {
            kc = ctx->keymap[layer][i];
            ctx->active_code[i] = kc;
        }

        if (kc >= KC_MEDIA_NEXT && kc <= KC_MEDIA_BRIGHTNESS_DOWN) {
            ctx->consumer_report = consumer_usages[kc - KC_MEDIA_NEXT];
            continue;
        }
        if (kc == KB_KEY_CHORD) {
            modifiers = (uint8_t)(ctx->active_code[i] >> 16);
            kc = (uint8_t)(ctx->active_code[i] >> 8);
        }
        if (kc == 0U || kc == KC_FN || kc == KC_PN || kc == KC_MO3) {
            continue;
        }
        ctx->last_report.modifiers |= modifiers;
        ctx->last_nkro.modifiers |= modifiers;
        if (kc >= KC_LCTRL && kc <= KC_RGUI) {
            uint8_t mod_bit = (uint8_t)(1U << (kc - KC_LCTRL));
            ctx->last_report.modifiers |= mod_bit;
            ctx->last_nkro.modifiers |= mod_bit;
        } else {
            if (kc < 128U) {
                ctx->last_nkro.bitmap[kc >> 3U] |= (uint8_t)(1U << (kc & 0x07U));
            }
            /* Preserve 6KRO rollover order of previously held keys */
            is_old = false;
            for (n = 0U; n < 6U; n++) {
                if (prev_keys[n] != 0U && prev_keys[n] == kc) {
                    is_old = true;
                    for (m = 0U; m < bck_num; m++) {
                        if (data_bck[m] == kc) {
                            break;
                        }
                    }
                    if (m == bck_num && bck_num < 6U) {
                        data_bck[bck_num++] = kc;
                    }
                    break;
                }
            }
            if (!is_old) {
                for (m = 0U; m < new_num; m++) {
                    if (data_new[m] == kc) {
                        break;
                    }
                }
                if (m == new_num && new_num < 6U) {
                    data_new[new_num++] = kc;
                }
            }
        }
    }

    if (bck_num > 0U) {
        memcpy(&ctx->last_report.keys[0], data_bck, bck_num);
    }
    if (new_num > 0U && bck_num < 6U) {
        uint8_t copy_cnt = (uint8_t)((6U - bck_num < new_num) ? (6U - bck_num) : new_num);
        memcpy(&ctx->last_report.keys[bck_num], data_new, copy_cnt);
    }

    /* A macro is another source of held keys, not a replacement for physical keys. */
    ctx->last_report.modifiers |= ctx->macro_modifiers;
    ctx->last_nkro.modifiers |= ctx->macro_modifiers;
    for (i = 0U; i < 6U; i++) {
        uint8_t kc = ctx->macro_keys[i];
        if (kc == 0U) {
            continue;
        }
        if (kc < 128U) {
            ctx->last_nkro.bitmap[kc >> 3U] |= (uint8_t)(1U << (kc & 7U));
        }
        for (n = 0U; n < 6U; n++) {
            if (ctx->last_report.keys[n] == kc) {
                break;
            }
        }
        if (n == 6U) {
            for (n = 0U; n < 6U; n++) {
                if (ctx->last_report.keys[n] == 0U) {
                    ctx->last_report.keys[n] = kc;
                    break;
                }
            }
        }
    }

    if (ctx->report_cb != NULL) {
        ctx->report_cb(&ctx->last_report, ctx->report_priv);
    }
    if (ctx->nkro_cb != NULL) {
        ctx->nkro_cb(&ctx->last_nkro, ctx->report_priv);
    }
}

static int kb_get_desc(struct nlk_node *node, struct nlk_capability *cap, void *buf,
                       uint16_t max_len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;

    (void)node;
    if (buf == NULL || max_len < sizeof(ctx->desc)) {
        return -ENOSPC;
    }
    memcpy(buf, &ctx->desc, sizeof(ctx->desc));
    return (int)sizeof(ctx->desc);
}

static uint8_t kb_find_action_id(const struct nlk_keyboard_ctx *ctx, const char *name)
{
    uint8_t i;
    if (ctx == NULL || name == NULL || name[0] == '\0') {
        return 0U;
    }
    for (i = 0U; i < ctx->action_count && i < NLK_KB_MAX_ACTIONS; i++) {
        if (strcmp(ctx->action_names[i], name) == 0) {
            return nlk_cap_keyboard_action_id(ctx, i);
        }
    }
    return 0U;
}

static bool kb_macro_steps_valid(const struct nlk_kb_macro_step *steps, uint8_t count)
{
    uint8_t i;
    if (count > NLK_KB_MAX_MACRO_STEPS) {
        return false;
    }
    if (count > 0U && steps == NULL) {
        return false;
    }
    for (i = 0U; i < count; i++) {
        uint8_t type = steps[i].type;
        uint8_t kc = steps[i].key_code;
        if (type < NLK_KB_MACRO_ACT_KEY_DOWN || type > NLK_KB_MACRO_ACT_DELAY_MS ||
            (type != NLK_KB_MACRO_ACT_DELAY_MS &&
             (kc == 0U || kc >= KC_FN || kb_is_function_key(kc)))) {
            return false;
        }
    }
    return true;
}

static uint32_t kb_execute_action_chain(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                        uint8_t key_index, uint32_t initial_code, uint8_t pressed);

int nlk_cap_keyboard_validate_light_layer(const struct nlk_keyboard_ctx *ctx,
                                          const struct nlk_kb_light_layer *layer)
{
    uint16_t width;
    uint16_t height;

    if (ctx == NULL || layer == NULL) {
        return -EINVAL;
    }
    width = ctx->desc.led_matrix_cols != 0U ? ctx->desc.led_matrix_cols : ctx->desc.matrix_cols;
    height = ctx->desc.led_matrix_rows != 0U ? ctx->desc.led_matrix_rows : ctx->desc.matrix_rows;
    if (layer->width == 0U || layer->height == 0U || (uint16_t)layer->x + layer->width > width ||
        (uint16_t)layer->y + layer->height > height || layer->activation > NLK_KB_LIGHT_PULSE ||
        (layer->trigger == 0U) != (layer->activation == NLK_KB_LIGHT_ALWAYS) ||
        layer->trigger > 0xFFFFFFU) {
        return -EINVAL;
    }
    if (layer->zone_id != NLK_KB_LIGHT_LAYER_OWN_CFG) {
        return -EINVAL;
    }
    return nlk_cap_keyboard_find_light_effect(ctx, layer->cfg.effect_id) != NULL ? 0 : -EINVAL;
}

static uint32_t kb_next_trigger_seq(struct nlk_keyboard_ctx *ctx)
{
    uint32_t seq = ++ctx->light_trigger_seq;

    if (seq == 0U) {
        seq = ++ctx->light_trigger_seq;
    }
    return seq;
}

static bool kb_sync_action_states(struct nlk_keyboard_ctx *ctx, bool notify)
{
    bool changed = false;
    uint8_t i;

    if (ctx == NULL || ctx->action_state_cb == NULL || ctx->light_runtime == NULL) {
        return false;
    }
    for (i = 0U; i < ctx->light_layer_count; i++) {
        const struct nlk_kb_light_layer *layer = &ctx->light_layers[i];
        struct kb_action_state state = {0};
        struct kb_light_trigger_data *dst = &ctx->light_runtime->triggers[i];
        uint8_t next_active;

        if (layer->activation != NLK_KB_LIGHT_STATE || layer->trigger == 0U) {
            continue;
        }
        if (!ctx->action_state_cb(ctx, layer->trigger, &state, ctx->action_state_priv)) {
            continue;
        }
        next_active = state.active ? 1U : 0U;
        if (state.active != ctx->light_layer_active[i] || dst->value != state.value ||
            dst->event == KB_LIGHT_TRIG_NONE) {
            uint32_t prev_seq = dst->seq;
            bool rising = state.active && !ctx->light_layer_active[i];

            memset(dst, 0, sizeof(*dst));
            dst->event = KB_LIGHT_TRIG_STATE_SYNC;
            dst->active = next_active;
            dst->layer = KB_LIGHT_COORD_NONE;
            dst->key_index = KB_LIGHT_COORD_NONE;
            dst->key_x = KB_LIGHT_COORD_NONE;
            dst->key_y = KB_LIGHT_COORD_NONE;
            dst->value = state.value;
            dst->seq = rising ? kb_next_trigger_seq(ctx) : prev_seq;
            ctx->light_layer_active[i] = state.active;
            changed = true;
        }
    }
    if (notify && changed && ctx->light_cb != NULL) {
        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
    }
    return changed;
}

/* 将各灯效层的触发上下文复位为无事件、无坐标状态 */
static void kb_reset_light_triggers(struct nlk_keyboard_ctx *ctx)
{
    uint8_t i;

    if (ctx == NULL || ctx->light_runtime == NULL) {
        return;
    }
    memset(ctx->light_runtime->triggers, 0, sizeof(ctx->light_runtime->triggers));
    for (i = 0U; i < CONFIG_KEYBOARD_LIGHT_MAX_LAYERS; i++) {
        ctx->light_runtime->triggers[i].layer = KB_LIGHT_COORD_NONE;
        ctx->light_runtime->triggers[i].key_index = KB_LIGHT_COORD_NONE;
        ctx->light_runtime->triggers[i].key_x = KB_LIGHT_COORD_NONE;
        ctx->light_runtime->triggers[i].key_y = KB_LIGHT_COORD_NONE;
    }
}

int nlk_cap_keyboard_set_light_layers(struct nlk_keyboard_ctx *ctx,
                                      const struct nlk_kb_light_layer *layers, uint8_t count)
{
    uint8_t i;

    if (ctx == NULL || (count != 0U && layers == NULL) ||
        count > CONFIG_KEYBOARD_LIGHT_MAX_LAYERS) {
        return -EINVAL;
    }
    for (i = 0U; i < count; i++) {
        if (nlk_cap_keyboard_validate_light_layer(ctx, &layers[i]) != 0) {
            return -EINVAL;
        }
    }
    if (count != 0U) {
        memmove(ctx->light_layers, layers, (size_t)count * sizeof(*layers));
    }
    ctx->light_layer_count = count;
    kb_reset_light_triggers(ctx);
    for (i = 0U; i < CONFIG_KEYBOARD_LIGHT_MAX_LAYERS; i++) {
        ctx->light_layer_active[i] = i < count && ctx->light_layers[i].trigger == 0U;
    }
    /* 新图层加载后立即同步状态类图层，避免上电/切换配置时与真实状态反相 */
    (void)kb_sync_action_states(ctx, false);
    ctx->light_layer_generation++;
    pr_info("light layers=%u generation=%u\n", count, ctx->light_layer_generation);
    if (ctx->light_cb != NULL) {
        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
    }
    return 0;
}

void nlk_cap_keyboard_set_action_state_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_action_state_fn_t cb,
                                          void *priv)
{
    if (ctx == NULL) {
        return;
    }
    ctx->action_state_cb = cb;
    ctx->action_state_priv = priv;
    (void)kb_sync_action_states(ctx, true);
}

void nlk_cap_keyboard_sync_action_states(struct nlk_keyboard_ctx *ctx)
{
    (void)kb_sync_action_states(ctx, true);
}

void nlk_cap_keyboard_set_indicators(struct nlk_keyboard_ctx *ctx, uint8_t indicator_leds)
{
    if (ctx == NULL || ctx->indicator_leds == indicator_leds) {
        return;
    }
    ctx->indicator_leds = indicator_leds;
    if (!kb_sync_action_states(ctx, true) && ctx->light_cb != NULL) {
        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
    }
}

void nlk_cap_keyboard_notify_light_trigger(struct nlk_keyboard_ctx *ctx, const char *action,
                                           const struct kb_light_trigger_data *trigger)
{
    uint32_t trig_hash;
    struct kb_action_state query = {0};
    bool has_query = false;
    bool query_checked = false;
    bool changed = false;
    uint8_t i;

    if (ctx == NULL || action == NULL || trigger == NULL || ctx->light_runtime == NULL) {
        return;
    }
    trig_hash = nlk_fnv1a_24(action);
    for (i = 0U; i < ctx->light_layer_count; i++) {
        const struct nlk_kb_light_layer *layer = &ctx->light_layers[i];
        bool prev_active = ctx->light_layer_active[i];
        bool active = prev_active;
        bool bump_seq = false;
        struct kb_light_trigger_data payload = *trigger;

        if (layer->trigger != trig_hash) {
            continue;
        }
        if (layer->activation == NLK_KB_LIGHT_STATE && !query_checked) {
            query_checked = true;
            if (ctx->action_state_cb != NULL) {
                has_query = ctx->action_state_cb(ctx, trig_hash, &query, ctx->action_state_priv);
            }
        }
        if (layer->activation == NLK_KB_LIGHT_HOLD) {
            active = (trigger->event == KB_LIGHT_TRIG_STATE_SYNC)
                         ? (trigger->active != 0U)
                         : (trigger->event == KB_LIGHT_TRIG_KEY_DOWN);
            bump_seq = active && !prev_active;
        } else if (layer->activation == NLK_KB_LIGHT_TOGGLE) {
            if (trigger->event == KB_LIGHT_TRIG_KEY_DOWN) {
                active = !active;
                bump_seq = active;
            }
        } else if (layer->activation == NLK_KB_LIGHT_STATE) {
            active = has_query ? query.active : (trigger->active != 0U);
            payload.active = active ? 1U : 0U;
            if (has_query) {
                payload.value = query.value;
            }
            bump_seq = active && !prev_active;
        } else if (layer->activation == NLK_KB_LIGHT_PULSE) {
            if (trigger->event == KB_LIGHT_TRIG_KEY_DOWN ||
                (trigger->event == KB_LIGHT_TRIG_STATE_SYNC &&
                 (has_query ? query.active : (trigger->active != 0U)))) {
                active = true;
                bump_seq = true;
            }
        }
        payload.seq = bump_seq ? kb_next_trigger_seq(ctx) : ctx->light_runtime->triggers[i].seq;
        if (memcmp(&ctx->light_runtime->triggers[i], &payload, sizeof(payload)) != 0) {
            ctx->light_runtime->triggers[i] = payload;
            changed = true;
        }
        if (active != prev_active || bump_seq) {
            pr_info("light layer=%u zone=%u action=%s active=%u\n", i, layer->zone_id, action,
                    active ? 1U : 0U);
            ctx->light_layer_active[i] = active;
            changed = true;
        }
    }
    if (changed && ctx->light_cb != NULL) {
        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
    }
}

/* Configuration RPC, events, streams and CORE descriptor share one capability. */
static int kb_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                     const struct nlk_pkt_hdr *hdr, const void *req, uint16_t req_len, void *resp,
                     uint16_t *resp_len)
{
    struct nlk_keyboard_ctx *ctx = cap->priv;

    if (ctx->config_ops == NULL) {
        return -EAGAIN;
    }
    return ctx->config_ops->rpc(node, hdr, req, req_len, resp, resp_len, ctx->config_priv);
}

static int kb_on_event(struct nlk_node *node, struct nlk_capability *cap,
                       const struct nlk_pkt_hdr *hdr, const void *evt, uint16_t evt_len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;

    (void)node;
    if (ctx == NULL || evt == NULL) {
        return -EINVAL;
    }

    if (hdr->cmd_op == NLK_KB_EVT_SWITCH_DELTA) {
        const struct nlk_kb_switch_event_hdr *ehdr = (const struct nlk_kb_switch_event_hdr *)evt;
        const struct nlk_kb_switch_item *items;
        uint8_t mod_offset = 0U;
        uint8_t i;

        if (evt_len < sizeof(*ehdr) ||
            evt_len < sizeof(*ehdr) + (size_t)ehdr->event_count * sizeof(*items)) {
            return -EINVAL;
        }
        if (ehdr->module_id > 0U && ehdr->module_id < 32U) {
            ctx->desc.ext_module_mask |= (1UL << ehdr->module_id);
        }
        if (ehdr->module_id < 8U) {
            mod_offset = ctx->module_key_offset[ehdr->module_id];
        }
        items = (const struct nlk_kb_switch_item *)((const uint8_t *)evt + sizeof(*ehdr));
        for (i = 0U; i < ehdr->event_count; i++) {
            uint16_t idx = (uint16_t)mod_offset + items[i].key_index;
            if (idx < ctx->desc.key_count) {
                uint8_t old_st = ctx->key_state[idx];
                ctx->key_state[idx] = items[i].state;
                if (items[i].state == 0U) {
                    if (old_st != 0U && ctx->active_code[idx] != KB_KEY_SUPPRESSED) {
                        uint32_t code =
                            ctx->active_code[idx] == KB_KEY_DROPPED ? 0U : ctx->active_code[idx];
                        (void)kb_execute_action_chain(ctx, ehdr->active_layer, (uint8_t)idx, code,
                                                      0U);
                    }
                    ctx->active_code[idx] = 0U;
                } else {
                    if (old_st == 0U) {
                        uint32_t final_kc = kb_execute_action_chain(
                            ctx, ehdr->active_layer, (uint8_t)idx, (uint32_t)items[i].key_code, 1U);
                        ctx->active_code[idx] =
                            final_kc == 0U && items[i].key_code != 0U ? KB_KEY_DROPPED : final_kc;
                    }
                }
                if (items[i].key_code != 0U && ctx->keymap[0][idx] == 0U) {
                    ctx->keymap[0][idx] = (uint32_t)items[i].key_code;
                }
            }
        }
        kb_rebuild_hid_report(ctx);
        return 0;
    }

    if (hdr->cmd_op == NLK_KB_EVT_HID_REPORT) {
        if (evt_len < sizeof(struct nlk_kb_hid_report)) {
            return -EINVAL;
        }
        memcpy(&ctx->last_report, evt, sizeof(ctx->last_report));
        if (ctx->report_cb != NULL) {
            ctx->report_cb(&ctx->last_report, ctx->report_priv);
        }
        return 0;
    }

    if (hdr->cmd_op == NLK_KB_EVT_NKRO_REPORT) {
        if (evt_len < sizeof(struct nlk_kb_nkro_report)) {
            return -EINVAL;
        }
        memcpy(&ctx->last_nkro, evt, sizeof(ctx->last_nkro));
        if (ctx->nkro_cb != NULL) {
            ctx->nkro_cb(&ctx->last_nkro, ctx->report_priv);
        }
        return 0;
    }

    if (hdr->cmd_op == NLK_KB_EVT_LIGHT_SYNC_BEACON) {
        if (evt_len < sizeof(ctx->last_light_beacon)) {
            return -EINVAL;
        }
        memcpy(&ctx->last_light_beacon, evt, sizeof(ctx->last_light_beacon));
        if (ctx->light_effect_count == 0U) {
            ctx->light_cfg.effect_id = ctx->last_light_beacon.effect_id;
        } else if (evt_len >= sizeof(struct nlk_kb_named_light_sync_beacon)) {
            const struct nlk_kb_named_light_sync_beacon *named = evt;
            const struct nlk_kb_effect_entry *entry;
            for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
                if (strncmp(named->effect_name, nlk_kb_effect_name(entry),
                            NLK_KB_LIGHT_EFFECT_NAME_MAX) == 0) {
                    ctx->light_cfg.effect_id = nlk_kb_effect_id(entry);
                    break;
                }
            }
        }
        ctx->light_cfg.speed = ctx->last_light_beacon.speed;
        ctx->light_cfg.brightness = ctx->last_light_beacon.brightness;
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, &ctx->last_light_beacon, ctx->indicator_leds,
                          ctx->light_priv);
        }
        return 0;
    }

    return -ENOTSUP;
}

static int kb_on_stream(struct nlk_node *node, struct nlk_capability *cap,
                        const struct nlk_pkt_hdr *hdr, const void *data, uint16_t len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;
    const struct nlk_kb_light_frame_hdr *fhdr = (const struct nlk_kb_light_frame_hdr *)data;

    (void)node;
    if (ctx == NULL || data == NULL || hdr->cmd_op != NLK_KB_STREAM_LIGHT_FRAME ||
        len < sizeof(*fhdr)) {
        return -EINVAL;
    }
    if (ctx->light_frame_cb != NULL) {
        ctx->light_frame_cb(fhdr->start_led, fhdr->led_count, (const uint8_t *)data + sizeof(*fhdr),
                            (uint16_t)(len - sizeof(*fhdr)), ctx->light_priv);
    }
    return 0;
}

#define KB_LIGHT_BEACON_INTERVAL_MS 200U

static void kb_on_poll(struct nlk_node *node, struct nlk_capability *cap, uint32_t now_ms)
{
    struct nlk_keyboard_ctx *ctx = cap->priv;

    if (ctx->config_ops != NULL) {
        ctx->config_ops->poll(ctx->config_priv, now_ms);
    }
    if (node->desc.attached_children != 0U &&
        (int32_t)(now_ms - ctx->next_beacon_ms) >= 0) {
        ctx->next_beacon_ms = now_ms + KB_LIGHT_BEACON_INTERVAL_MS;
        (void)nlk_cap_keyboard_broadcast_light_beacon(ctx, now_ms);
    }
}

static const struct nlk_cap_ops g_kb_ops = {
    .on_poll = kb_on_poll,
    .get_desc = kb_get_desc,
    .on_rpc = kb_on_rpc,
    .on_event = kb_on_event,
    .on_stream = kb_on_stream,
};

static void kb_macro_key(struct nlk_keyboard_ctx *ctx, uint8_t kc, bool down)
{
    uint8_t i;
    if (kc >= KC_LCTRL && kc <= KC_RGUI) {
        uint8_t bit = (uint8_t)(1U << (kc - KC_LCTRL));
        if (down) {
            ctx->macro_modifiers |= bit;
        } else {
            ctx->macro_modifiers &= (uint8_t)~bit;
        }
        return;
    }
    if (kc == 0U || kb_is_function_key(kc)) {
        return;
    }
    for (i = 0U; i < 6U; i++) {
        if (ctx->macro_keys[i] == kc) {
            break;
        }
    }
    if (down && i == 6U) {
        for (i = 0U; i < 6U; i++) {
            if (ctx->macro_keys[i] == 0U) {
                ctx->macro_keys[i] = kc;
                break;
            }
        }
    } else if (!down && i < 6U) {
        ctx->macro_keys[i] = 0U;
    }
}

static bool kb_macro_has_held_keys(const struct nlk_keyboard_ctx *ctx)
{
    uint8_t i;
    if (ctx->macro_modifiers != 0U) {
        return true;
    }
    for (i = 0U; i < 6U; i++) {
        if (ctx->macro_keys[i] != 0U) {
            return true;
        }
    }
    return false;
}

int nlk_cap_keyboard_macro_cancel(struct nlk_keyboard_ctx *ctx)
{
    bool held;
    bool was_playing;
    if (ctx == NULL) {
        return -EINVAL;
    }
    was_playing = ctx->macro_playing != 0U;
    held = kb_macro_has_held_keys(ctx);
    ctx->macro_playing = 0U;
    ctx->macro_tap_pending = 0U;
    ctx->macro_modifiers = 0U;
    memset(ctx->macro_keys, 0, sizeof(ctx->macro_keys));
    if (held) {
        kb_rebuild_hid_report(ctx);
    }
    if (was_playing) {
        nlk_cap_keyboard_sync_action_states(ctx);
    }
    return 0;
}

int nlk_cap_keyboard_macro_start(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, uint32_t now_ms)
{
    const struct nlk_kb_macro *macro;
    uint8_t count;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->macro_playing != 0U) {
        return -EBUSY;
    }
    macro = nlk_cap_keyboard_find_macro(ctx, macro_id);
    if (macro == NULL || macro->info.step_count == 0U) {
        return -EINVAL;
    }
    count = macro->info.step_count;
    memcpy(ctx->macro_play_steps, macro->steps, (size_t)count * sizeof(macro->steps[0]));
    ctx->macro_play_count = count;
    ctx->macro_play_index = 0U;
    ctx->macro_play_id = macro_id;
    ctx->macro_repeats_left = macro->info.repeat_count;
    if (ctx->macro_repeats_left == 0U) {
        ctx->macro_repeats_left = 1U;
    }
    ctx->macro_tap_pending = 0U;
    ctx->macro_playing = 1U;
    ctx->macro_clock_ms = now_ms;
    ctx->macro_due_ms = now_ms + ctx->macro_play_steps[0].delay_ms;
    nlk_cap_keyboard_sync_action_states(ctx);
    return 0;
}

int nlk_cap_keyboard_tick(struct nlk_keyboard_ctx *ctx, uint32_t now_ms)
{
    const struct nlk_kb_macro_step *step;
    uint32_t delay;
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->macro_clock_ms = now_ms;
    if (ctx->macro_playing == 0U || (int32_t)(now_ms - ctx->macro_due_ms) < 0) {
        return 0;
    }
    step = &ctx->macro_play_steps[ctx->macro_play_index];
    if (ctx->macro_tap_pending != 0U) {
        kb_macro_key(ctx, step->key_code, false);
        ctx->macro_tap_pending = 0U;
        kb_rebuild_hid_report(ctx);
    } else if (step->type == NLK_KB_MACRO_ACT_KEY_TAP) {
        kb_macro_key(ctx, step->key_code, true);
        kb_rebuild_hid_report(ctx);
        ctx->macro_tap_pending = 1U;
        ctx->macro_due_ms = now_ms + 1U;
        return 0;
    } else if (step->type == NLK_KB_MACRO_ACT_KEY_DOWN || step->type == NLK_KB_MACRO_ACT_KEY_UP) {
        kb_macro_key(ctx, step->key_code, step->type == NLK_KB_MACRO_ACT_KEY_DOWN);
        kb_rebuild_hid_report(ctx);
    }
    ctx->macro_play_index++;
    if (ctx->macro_play_index >= ctx->macro_play_count) {
        ctx->macro_repeats_left--;
        if (ctx->macro_repeats_left == 0U) {
            return nlk_cap_keyboard_macro_cancel(ctx);
        }
        /* Do not carry an unfinished chord into the next repetition. */
        if (kb_macro_has_held_keys(ctx)) {
            ctx->macro_modifiers = 0U;
            memset(ctx->macro_keys, 0, sizeof(ctx->macro_keys));
            kb_rebuild_hid_report(ctx);
        }
        ctx->macro_play_index = 0U;
    }
    delay = ctx->macro_play_steps[ctx->macro_play_index].delay_ms;
    ctx->macro_due_ms = now_ms + (delay == 0U ? 1U : delay);
    return 0;
}

static uint32_t kb_execute_action_chain(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                        uint8_t key_index, uint32_t initial_code, uint8_t pressed)
{
    struct nlk_kb_key_action_chain *chain;
    struct key_action_context act_ctx;
    uint32_t current_code = initial_code;
    uint32_t final_code = initial_code;
    bool key_dropped = false;
    uint8_t k, a;

    if (ctx == NULL || layer >= ctx->desc.layer_count || key_index >= ctx->desc.key_count) {
        return initial_code;
    }

    chain = &ctx->action_chains[layer][key_index];
    if (chain->count == 0U) {
        return initial_code;
    }

    act_ctx.layer = layer;
    act_ctx.key_index = key_index;
    act_ctx.initial_code = initial_code;
    act_ctx.event_type = pressed ? 1U : 0U;
    act_ctx.state = pressed ? 1U : 0U;

    for (k = 0U; k < chain->count && k < NLK_KB_MAX_KEY_ACTIONS; k++) {
        uint8_t act_id = chain->action_ids[k];
        act_ctx.action_slot = k;
        for (a = 0U; a < ctx->action_count && a < NLK_KB_MAX_ACTIONS; a++) {
            if (nlk_cap_keyboard_action_id(ctx, a) == act_id) {
                int res = KEY_ACTION_PASS;
                ctx->action_trigger_published = false;
                if (ctx->action_handlers[a] != NULL) {
                    act_ctx.key_code = current_code; /* 收集上一个动作输出的按键值作为输入 */
                    res = ctx->action_handlers[a](&act_ctx, ctx->action_handler_privs[a]);

                    /* 在使用时判断动作回调是否返回了按键值 */
                    if (res < 0) {
                        /* 动作明确请求拦截按键 (如宏播放、Profile 切换等) */
                        key_dropped = true;
                        current_code = 0U;
                        final_code = 0U;
                    } else if (res > 0) {
                        /* 动作输出了有效按键值，以最后一个输出的为准 */
                        current_code = (uint32_t)res;
                        final_code = (uint32_t)res;
                        key_dropped = false;
                    } else {
                        /* res == 0 (KEY_ACTION_PASS): 该动作未返回按键值 (如仅用于点灯/旁路通知)
                         * 不覆盖已有按键值，保持原有键码继续流转 */
                    }
                }
                if (!ctx->action_trigger_published) {
                    struct kb_light_trigger_data trig = {
                        .event = pressed != 0U ? KB_LIGHT_TRIG_KEY_DOWN : KB_LIGHT_TRIG_KEY_UP,
                        .active = pressed != 0U ? 1U : 0U,
                        .layer = layer,
                        .key_index = key_index,
                        .key_x = KB_LIGHT_COORD_NONE,
                        .key_y = KB_LIGHT_COORD_NONE,
                        .key_code = current_code,
                        .value = res > 0 ? (uint32_t)res : 0U,
                    };
                    nlk_cap_keyboard_notify_light_trigger(ctx, ctx->action_names[a], &trig);
                }
                break;
            }
        }
    }
    return key_dropped ? 0U : final_code;
}

void nlk_cap_keyboard_deinit(struct nlk_keyboard_ctx *ctx)
{
    struct nlk_kb_effect_entry *entry;

    if (ctx == NULL) {
        return;
    }
    entry = ctx->light_effects;
    while (entry != NULL) {
        struct nlk_kb_effect_entry *next = entry->next;
        kfree(entry);
        entry = next;
    }
    ctx->light_effects = NULL;
    ctx->light_effects_tail = NULL;
    ctx->light_effect_count = 0U;
    if (ctx->keymap[0] != NULL) {
        kfree(ctx->keymap[0]);
    }
    if (ctx->action_chains[0] != NULL) {
        kfree(ctx->action_chains[0]);
    }
    if (ctx->key_state != NULL) {
        kfree(ctx->key_state);
    }
    if (ctx->active_code != NULL) {
        kfree(ctx->active_code);
    }
    if (ctx->light_runtime != NULL) {
        kfree(ctx->light_runtime);
        ctx->light_runtime = NULL;
    }
    memset(ctx->keymap, 0, sizeof(ctx->keymap));
    memset(ctx->action_chains, 0, sizeof(ctx->action_chains));
    nlk_cap_keyboard_free_macros(ctx->macros);
    ctx->macros = NULL;
    ctx->macro_count = 0U;
    ctx->key_state = NULL;
    ctx->active_code = NULL;
    ctx->key_capacity = 0U;
    ctx->layer_capacity = 0U;
}

int nlk_cap_keyboard_init(struct nlk_node *node, struct nlk_keyboard_ctx *ctx, uint8_t rows,
                          uint8_t cols, uint8_t key_count, nlk_kb_report_cb_t report_cb, void *priv)
{
    return nlk_cap_keyboard_init_sized(node, ctx, rows, cols, key_count, NLK_KB_MAX_LAYERS,
                                       report_cb, priv);
}

int nlk_cap_keyboard_init_sized(struct nlk_node *node, struct nlk_keyboard_ctx *ctx, uint8_t rows,
                                uint8_t cols, uint8_t key_count, uint8_t layer_count,
                                nlk_kb_report_cb_t report_cb, void *priv)
{
    uint8_t l;
    size_t cells;
    int ret;

    if (ctx == NULL || key_count == 0U || key_count > NLK_KB_MAX_KEYS || layer_count == 0U ||
        layer_count > NLK_KB_MAX_LAYERS) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    cells = (size_t)key_count * layer_count;
    ctx->keymap[0] = kmalloc(cells * sizeof(uint32_t), GFP_KERNEL);
    ctx->action_chains[0] = kmalloc(cells * sizeof(*ctx->action_chains[0]), GFP_KERNEL);
    ctx->key_state = kmalloc((size_t)key_count, GFP_KERNEL);
    ctx->active_code = kmalloc((size_t)key_count * sizeof(uint32_t), GFP_KERNEL);
    ctx->light_runtime = kmalloc(sizeof(*ctx->light_runtime), GFP_KERNEL);
    if (ctx->keymap[0] == NULL || ctx->action_chains[0] == NULL || ctx->key_state == NULL ||
        ctx->active_code == NULL || ctx->light_runtime == NULL) {
        nlk_cap_keyboard_deinit(ctx);
        return -ENOMEM;
    }
    memset(ctx->keymap[0], 0, cells * sizeof(uint32_t));
    memset(ctx->action_chains[0], 0, cells * sizeof(*ctx->action_chains[0]));
    memset(ctx->key_state, 0, (size_t)key_count);
    memset(ctx->active_code, 0, (size_t)key_count * sizeof(uint32_t));
    memset(ctx->light_runtime, 0, sizeof(*ctx->light_runtime));
    kb_reset_light_triggers(ctx);
    ctx->key_capacity = key_count;
    ctx->layer_capacity = layer_count;
    for (l = 1U; l < layer_count; l++) {
        ctx->keymap[l] = ctx->keymap[0] + (size_t)l * key_count;
        ctx->action_chains[l] = ctx->action_chains[0] + (size_t)l * key_count;
    }
    ctx->node = node;
    ctx->desc.form_factor = kb_infer_form_factor(key_count);
    ctx->desc.switch_type = NLK_KB_SW_MECH;
    ctx->desc.kb_flags = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO | NLK_KB_FLAG_MODULAR_HUB |
                         NLK_KB_FLAG_LIGHT_RGB_KEY | NLK_KB_FLAG_LIGHT_INDICATORS;
    ctx->desc.matrix_rows = rows;
    ctx->desc.matrix_cols = cols;
    ctx->desc.key_count = key_count;
    ctx->desc.layer_count = layer_count;
    ctx->desc.debounce_ms = 5U;
    ctx->desc.led_matrix_rows = rows;
    ctx->desc.led_matrix_cols = cols;
    ctx->desc.underglow_leds = 0U;
    ctx->desc.led_count = key_count;
    ctx->desc.poll_rate_hz = 1000U;

    ctx->active_layer = 0U;
    ctx->default_layer = 0U;
    ctx->active_layout_id = ctx->desc.form_factor;

    /* Initialize default parameters */
    ctx->params.debounce_ms = 5U;
    ctx->params.poll_rate_hz = 1000U;
    ctx->params.poll_rate_hz_div = 1U;
    ctx->params.nkro_enable = 1U;
    ctx->params.win_lock = 0U;
    ctx->params.fn_lock = 0U;
    ctx->params.sleep_timeout_min = 0U;
    ctx->params.active_layout_id = ctx->active_layout_id;
    ctx->params.default_layer = 0U;

    ctx->analog_cfg.actuation_01mm = 20U;
    ctx->analog_cfg.rapid_trigger_en = 0U;
    ctx->analog_cfg.rt_press_01mm = 3U;
    ctx->analog_cfg.rt_release_01mm = 3U;
    ctx->analog_cfg.bottom_deadzone = 2U;
    ctx->analog_cfg.top_deadzone = 2U;

    ctx->light_cfg.enable = 1U;
    ctx->light_cfg.zone_mask = NLK_KB_LIGHT_ZONE_ALL;
    ctx->light_cfg.effect_id = 0U;
    ctx->light_cfg.brightness = 200U;
    ctx->light_cfg.speed = 128U;

    ctx->report_cb = report_cb;
    ctx->report_priv = priv;

    ctx->cap.cap_ver = 0x20U;
    ctx->cap.cap_type = NLK_CAP_KEYBOARD;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_EVENT | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = ctx->desc.form_factor;
    ctx->cap.desc_size = sizeof(struct nlk_kb_desc);
    ctx->cap.name = "keyboard";
    ctx->cap.ops = &g_kb_ops;
    ctx->cap.priv = ctx;

    if (node == NULL) {
        return 0;
    }
    ret = nlk_cap_keyboard_attach(ctx, node);
    if (ret < 0) {
        nlk_cap_keyboard_deinit(ctx);
    }
    return ret;
}

int nlk_cap_keyboard_attach(struct nlk_keyboard_ctx *ctx, struct nlk_node *node)
{
    int ret;

    if (ctx == NULL || node == NULL || (ctx->node != NULL && ctx->node != node)) {
        return -EINVAL;
    }
    ctx->node = node;
    ret = nlk_node_register_cap(node, &ctx->cap);
    if (ret < 0) {
        ctx->node = NULL;
    }
    return ret;
}

int nlk_cap_keyboard_configure(struct nlk_keyboard_ctx *ctx, const struct nlk_kb_desc *desc)
{
    if (ctx == NULL || desc == NULL || desc->key_count == 0U ||
        desc->key_count > ctx->key_capacity || desc->layer_count == 0U ||
        desc->layer_count > ctx->layer_capacity) {
        return -EINVAL;
    }
    ctx->desc = *desc;
    ctx->cap.sub_class = desc->form_factor;
    return 0;
}

int nlk_cap_keyboard_set_light_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_light_cb_t light_cb,
                                  nlk_kb_light_frame_cb_t frame_cb, void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->light_cb = light_cb;
    ctx->light_frame_cb = frame_cb;
    ctx->light_priv = priv;
    return 0;
}

int nlk_cap_keyboard_set_zone_light_cb(struct nlk_keyboard_ctx *ctx,
                                       nlk_kb_zone_light_cb_t zone_light_cb, void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->zone_light_cb = zone_light_cb;
    ctx->zone_light_priv = priv;
    return 0;
}

int nlk_cap_keyboard_set_param_cb(struct nlk_keyboard_ctx *ctx, nlk_kb_param_change_cb_t param_cb,
                                  void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->param_cb = param_cb;
    ctx->param_priv = priv;
    return 0;
}

int nlk_cap_keyboard_register_layout(struct nlk_keyboard_ctx *ctx,
                                     const struct nlk_kb_layout_info *layout)
{
    if (ctx == NULL || layout == NULL || ctx->layout_count >= NLK_KB_MAX_LAYOUTS) {
        return -EINVAL;
    }
    if (ctx->layout_count == 0U) {
        ctx->active_layout_id = layout->layout_id;
        ctx->params.active_layout_id = layout->layout_id;
    }
    ctx->layout_count++;
    return 0;
}

const struct nlk_kb_effect_entry *
nlk_cap_keyboard_find_light_effect(const struct nlk_keyboard_ctx *ctx, uint8_t effect_id)
{
    const struct nlk_kb_effect_entry *entry;

    if (ctx == NULL) {
        return NULL;
    }
    for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
        if (nlk_kb_effect_id(entry) == effect_id) {
            return entry;
        }
    }
    return NULL;
}

int nlk_cap_keyboard_register_light_effect(struct nlk_keyboard_ctx *ctx, const char *name,
                                           const char *desc, nlk_kb_effect_render_fn_t handler,
                                           void *priv)
{
    struct nlk_kb_effect_entry *entry;

    if (ctx == NULL || name == NULL || name[0] == '\0' ||
        strlen(name) >= NLK_KB_LIGHT_EFFECT_NAME_MAX) {
        return -EINVAL;
    }
    for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
        if (strcmp(nlk_kb_effect_name(entry), name) == 0) {
            break;
        }
    }
    if (entry == NULL) {
        if (ctx->light_effect_count >= 256U) {
            return -ENOSPC; /* effect_id is an 8-bit wire field */
        }
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (entry == NULL) {
            return -ENOMEM;
        }
        memset(entry, 0, sizeof(*entry));
        entry->effect_id = (uint8_t)ctx->light_effect_count;
        entry->name = name;
        (void)desc;
        entry->render = handler;
        entry->priv = priv;
        if (ctx->light_effects_tail != NULL) {
            ctx->light_effects_tail->next = entry;
        } else {
            ctx->light_effects = entry;
        }
        ctx->light_effects_tail = entry;
        ctx->light_effect_count++;
        return 0;
    }
    entry->render = handler;
    entry->priv = priv;
    return 0;
}

int nlk_cap_keyboard_register_light_zone(struct nlk_keyboard_ctx *ctx,
                                         const struct nlk_kb_light_zone_desc *zone,
                                         const struct nlk_kb_zone_light_cfg *initial_cfg)
{
    if (ctx == NULL || zone == NULL || ctx->light_zone_count >= NLK_KB_MAX_LIGHT_ZONES) {
        return -EINVAL;
    }
    ctx->light_zones[ctx->light_zone_count] = *zone;
    ctx->light_zones[ctx->light_zone_count].zone_id = ctx->light_zone_count;
    if (initial_cfg != NULL) {
        ctx->zone_cfgs[ctx->light_zone_count] = *initial_cfg;
        ctx->zone_cfgs[ctx->light_zone_count].zone_id = ctx->light_zone_count;
    } else {
        memset(&ctx->zone_cfgs[ctx->light_zone_count], 0, sizeof(struct nlk_kb_zone_light_cfg));
        ctx->zone_cfgs[ctx->light_zone_count].zone_id = ctx->light_zone_count;
        ctx->zone_cfgs[ctx->light_zone_count].enable = 1U;
        ctx->zone_cfgs[ctx->light_zone_count].brightness = 200U;
    }
    ctx->light_zone_count++;
    return 0;
}

int nlk_cap_keyboard_register_action(struct nlk_keyboard_ctx *ctx, const char *name,
                                     const char *desc, key_action_handler_fn_t handler, void *priv)
{
    uint8_t i;

    if (ctx == NULL || name == NULL || name[0] == '\0' || strlen(name) >= 32U) {
        return -EINVAL;
    }
    for (i = 0U; i < ctx->action_count; i++) {
        if (strcmp(ctx->action_names[i], name) == 0) {
            /* Refreshing a label must preserve the registered callback and context. */
            if (handler != NULL) {
                ctx->action_handlers[i] = handler;
                ctx->action_handler_privs[i] = priv;
            }
            return 0;
        }
    }
    if (ctx->action_count >= NLK_KB_MAX_ACTIONS) {
        return -ENOSPC;
    }
    i = ctx->action_count;
    (void)desc; /* Definition descriptions are read directly from flash by v2 RPC. */
    ctx->action_names[i] = name;
    ctx->action_handlers[i] = handler;
    ctx->action_handler_privs[i] = priv;
    ctx->action_count++;
    return 0;
}

int nlk_cap_keyboard_bind_key_action(struct nlk_keyboard_ctx *ctx, uint8_t layer, uint8_t key_index,
                                     const char *action_name, bool append)
{
    uint8_t act_id;
    uint8_t l_start, l_end, l;
    uint8_t k_start, k_end, k;

    if (ctx == NULL || action_name == NULL) {
        return -EINVAL;
    }
    act_id = kb_find_action_id(ctx, action_name);
    if (act_id == 0U) {
        return -ENOENT;
    }

    if (layer == 0xFFU) {
        l_start = 0U;
        l_end = ctx->desc.layer_count;
    } else {
        if (layer >= ctx->desc.layer_count) {
            return -EINVAL;
        }
        l_start = layer;
        l_end = (uint8_t)(layer + 1U);
    }

    if (key_index == 0xFFU) {
        k_start = 0U;
        k_end = ctx->desc.key_count;
    } else {
        if (key_index >= ctx->desc.key_count) {
            return -EINVAL;
        }
        k_start = key_index;
        k_end = (uint8_t)(key_index + 1U);
    }

    for (l = l_start; l < l_end; l++) {
        for (k = k_start; k < k_end; k++) {
            struct nlk_kb_key_action_chain *chain = &ctx->action_chains[l][k];
            if (!append) {
                chain->count = 0U;
            }
            if (chain->count < NLK_KB_MAX_KEY_ACTIONS) {
                uint8_t idx;
                bool exists = false;
                for (idx = 0U; idx < chain->count; idx++) {
                    if (chain->action_ids[idx] == act_id) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    chain->action_ids[chain->count++] = act_id;
                }
            }
        }
    }
    return 0;
}

int nlk_cap_keyboard_unbind_key_action(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                       uint8_t key_index, const char *action_name)
{
    uint8_t act_id = 0U;
    uint8_t l_start, l_end, l;
    uint8_t k_start, k_end, k;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (action_name != NULL && action_name[0] != '\0') {
        act_id = kb_find_action_id(ctx, action_name);
        if (act_id == 0U) {
            return -ENOENT;
        }
    }

    if (layer == 0xFFU) {
        l_start = 0U;
        l_end = ctx->desc.layer_count;
    } else {
        if (layer >= ctx->desc.layer_count) {
            return -EINVAL;
        }
        l_start = layer;
        l_end = (uint8_t)(layer + 1U);
    }

    if (key_index == 0xFFU) {
        k_start = 0U;
        k_end = ctx->desc.key_count;
    } else {
        if (key_index >= ctx->desc.key_count) {
            return -EINVAL;
        }
        k_start = key_index;
        k_end = (uint8_t)(key_index + 1U);
    }

    for (l = l_start; l < l_end; l++) {
        for (k = k_start; k < k_end; k++) {
            struct nlk_kb_key_action_chain *chain = &ctx->action_chains[l][k];
            if (act_id == 0U) {
                chain->count = 0U;
            } else {
                uint8_t idx;
                for (idx = 0U; idx < chain->count; idx++) {
                    if (chain->action_ids[idx] == act_id) {
                        uint8_t m;
                        for (m = idx; m + 1U < chain->count; m++) {
                            chain->action_ids[m] = chain->action_ids[m + 1U];
                        }
                        chain->count--;
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

int nlk_cap_keyboard_bind_all_keys(struct nlk_keyboard_ctx *ctx, uint8_t layer_mask,
                                   const char *action_name, bool append)
{
    uint8_t l;
    if (ctx == NULL || action_name == NULL) {
        return -EINVAL;
    }
    for (l = 0U; l < ctx->desc.layer_count && l < 8U; l++) {
        if ((layer_mask & (uint8_t)(1U << l)) != 0U) {
            int ret = nlk_cap_keyboard_bind_key_action(ctx, l, 0xFFU, action_name, append);
            if (ret != 0) {
                return ret;
            }
        }
    }
    return 0;
}

struct nlk_kb_macro *nlk_cap_keyboard_find_macro(const struct nlk_keyboard_ctx *ctx, uint8_t id)
{
    struct nlk_kb_macro *macro;

    for (macro = ctx->macros; macro != NULL; macro = macro->next) {
        if (macro->info.macro_id == id) {
            return macro;
        }
    }
    return NULL;
}

struct nlk_kb_macro *nlk_cap_keyboard_alloc_macro(uint8_t capacity)
{
    size_t size = sizeof(struct nlk_kb_macro) + (size_t)capacity * sizeof(struct nlk_kb_macro_step);
    struct nlk_kb_macro *macro = kmalloc(size, GFP_KERNEL);

    if (macro != NULL) {
        memset(macro, 0, size);
    }
    return macro;
}

void nlk_cap_keyboard_free_macros(struct nlk_kb_macro *macro)
{
    while (macro != NULL) {
        struct nlk_kb_macro *next = macro->next;
        kfree(macro);
        macro = next;
    }
}

uint8_t nlk_cap_keyboard_next_macro_id(const struct nlk_keyboard_ctx *ctx)
{
    uint8_t id;
    for (id = 0U; id != NLK_KB_MACRO_NEW; id++) {
        if (nlk_cap_keyboard_find_macro(ctx, id) == NULL) {
            break;
        }
    }
    return id;
}

void nlk_cap_keyboard_replace_macros(struct nlk_keyboard_ctx *ctx, struct nlk_kb_macro *macros,
                                     uint8_t count)
{
    (void)nlk_cap_keyboard_macro_cancel(ctx);
    nlk_cap_keyboard_free_macros(ctx->macros);
    ctx->macros = macros;
    ctx->macro_count = count;
}

static void kb_install_macro(struct nlk_keyboard_ctx *ctx, uint8_t id, struct nlk_kb_macro *macro)
{
    struct nlk_kb_macro **link = &ctx->macros;
    struct nlk_kb_macro *old;
    addr_t flags = disable_irq_save();

    while (*link != NULL && (*link)->info.macro_id < id) {
        link = &(*link)->next;
    }
    old = *link;
    if (old != NULL && old->info.macro_id == id) {
        *link = old->next;
        ctx->macro_count--;
    } else {
        old = NULL;
    }
    if (macro != NULL) {
        macro->next = *link;
        *link = macro;
        ctx->macro_count++;
    }
    enable_irq_save(flags);
    if (old != NULL) {
        kfree(old);
    }
}

int nlk_cap_keyboard_set_macro_meta(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, const char *id,
                                    const char *name, const char *desc,
                                    const struct nlk_kb_macro_step *steps, uint8_t step_count,
                                    uint8_t repeat_count)
{
    struct nlk_kb_macro *macro;

    if (ctx == NULL || macro_id == NLK_KB_MACRO_NEW || !kb_macro_steps_valid(steps, step_count)) {
        return -EINVAL;
    }
    macro = nlk_cap_keyboard_alloc_macro(step_count);
    if (macro == NULL) {
        return -ENOMEM;
    }
    macro->info.macro_id = macro_id;
    macro->info.step_count = step_count;
    macro->info.repeat_count = repeat_count != 0U ? repeat_count : 1U;
    if (id != NULL) {
        strncpy(macro->info.id, id, sizeof(macro->info.id) - 1U);
    }
    if (name != NULL) {
        strncpy(macro->info.name, name, sizeof(macro->info.name) - 1U);
    }
    if (desc != NULL) {
        strncpy(macro->desc, desc, sizeof(macro->desc) - 1U);
    }
    if (step_count != 0U) {
        memcpy(macro->steps, steps, (size_t)step_count * sizeof(*steps));
    }
    if (ctx->macro_playing && ctx->macro_play_id == macro_id) {
        (void)nlk_cap_keyboard_macro_cancel(ctx);
    }
    kb_install_macro(ctx, macro_id, macro);
    return 0;
}

int nlk_cap_keyboard_set_macro(struct nlk_keyboard_ctx *ctx, uint8_t macro_id, const char *name,
                               const struct nlk_kb_macro_step *steps, uint8_t step_count,
                               uint8_t repeat_count)
{
    return nlk_cap_keyboard_set_macro_meta(ctx, macro_id, NULL, name, "", steps, step_count,
                                           repeat_count);
}

int nlk_cap_keyboard_set_params(struct nlk_keyboard_ctx *ctx, const struct nlk_kb_params *params)
{
    if (ctx == NULL || params == NULL) {
        return -EINVAL;
    }
    ctx->params = *params;
    if (params->debounce_ms > 0U) {
        ctx->desc.debounce_ms = params->debounce_ms;
    }
    if (params->poll_rate_hz > 0U) {
        ctx->desc.poll_rate_hz = params->poll_rate_hz;
    }
    if (params->nkro_enable != 0U) {
        ctx->desc.kb_flags |= NLK_KB_FLAG_NKRO;
    } else {
        ctx->desc.kb_flags &= (uint16_t)~NLK_KB_FLAG_NKRO;
    }
    if (ctx->param_cb != NULL) {
        (void)ctx->param_cb(&ctx->params, ctx->param_priv);
    }
    nlk_cap_keyboard_sync_action_states(ctx);
    return 0;
}

int nlk_cap_keyboard_set_module_offset(struct nlk_keyboard_ctx *ctx, uint8_t module_id,
                                       uint8_t key_offset)
{
    if (ctx == NULL || module_id >= 8U || key_offset >= NLK_KB_MAX_KEYS) {
        return -EINVAL;
    }
    ctx->module_key_offset[module_id] = key_offset;
    return 0;
}

int nlk_cap_keyboard_load_layout(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                 const uint8_t *key_codes, uint8_t count)
{
    uint8_t i;
    if (ctx == NULL || layer >= ctx->desc.layer_count || count > ctx->desc.key_count) {
        return -EINVAL;
    }
    if (key_codes != NULL) {
        for (i = 0U; i < count; i++) {
            ctx->keymap[layer][i] = (uint32_t)key_codes[i];
        }
    }
    return 0;
}

int nlk_cap_keyboard_load_layout32(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                   const uint32_t *key_codes, uint8_t count)
{
    uint8_t i;
    if (ctx == NULL || layer >= ctx->desc.layer_count || count > ctx->desc.key_count) {
        return -EINVAL;
    }
    if (key_codes != NULL) {
        for (i = 0U; i < count; i++) {
            ctx->keymap[layer][i] = key_codes[i];
        }
    }
    return 0;
}

void nlk_cap_keyboard_release_held_keys(struct nlk_keyboard_ctx *ctx)
{
    uint8_t i;

    if (ctx == NULL) {
        return;
    }
    for (i = 0U; i < ctx->desc.key_count; i++) {
        if (ctx->key_state[i] != 0U) {
            ctx->active_code[i] = KB_KEY_SUPPRESSED;
        }
    }
    (void)nlk_cap_keyboard_macro_cancel(ctx);
    for (i = 0; i < ctx->light_layer_count; i++) {
        if (ctx->light_layers[i].activation == NLK_KB_LIGHT_HOLD) {
            ctx->light_layer_active[i] = false;
        }
    }
    if (ctx->light_cb) {
        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
    }
    kb_rebuild_hid_report(ctx);
}

int nlk_cap_keyboard_update_local_keys(struct nlk_keyboard_ctx *ctx, const uint32_t *raw_bitmap,
                                       uint8_t key_count)
{
    uint8_t limit;
    uint8_t layer;
    uint8_t i;

    if (ctx == NULL || raw_bitmap == NULL) {
        return -EINVAL;
    }
    limit = (key_count <= ctx->desc.key_count) ? key_count : ctx->desc.key_count;

    /* First pass: update raw key_state for layer-shift keys so active_layer is current */
    for (i = 0U; i < limit; i++) {
        uint8_t pressed = (uint8_t)((raw_bitmap[i / 32U] >> (i % 32U)) & 0x01U);
        uint8_t base_kc = (uint8_t)(ctx->keymap[0][i] & 0xFFU);
        if (ctx->active_code[i] != KB_KEY_SUPPRESSED &&
            (base_kc == KC_FN || base_kc == KC_PN || base_kc == KC_MO3)) {
            ctx->key_state[i] = pressed;
        }
    }
    layer = kb_resolve_active_layer(ctx);

    /* Second pass: process press/release transitions and fire key actions */
    for (i = 0U; i < limit; i++) {
        uint8_t pressed = (uint8_t)((raw_bitmap[i / 32U] >> (i % 32U)) & 0x01U);
        uint32_t base_code = ctx->keymap[0][i];
        uint8_t base_kc = (uint8_t)(base_code & 0xFFU);
        bool is_layer_key = (base_kc == KC_FN || base_kc == KC_PN || base_kc == KC_MO3);

        if (ctx->active_code[i] == KB_KEY_SUPPRESSED) {
            if (pressed == 0U) {
                ctx->key_state[i] = 0U;
                ctx->active_code[i] = 0U;
            }
            continue;
        }
        if (pressed != 0U) {
            if (ctx->key_state[i] == 0U || is_layer_key) {
                uint32_t code = is_layer_key ? base_code : ctx->keymap[layer][i];
                bool first_down = (ctx->key_state[i] == 0U && !is_layer_key);

                ctx->key_state[i] = 1U;
                if (first_down) {
                    uint32_t final_code = kb_execute_action_chain(ctx, layer, i, code, 1U);
                    ctx->active_code[i] =
                        final_code == 0U && code != 0U ? KB_KEY_DROPPED : final_code;
                } else {
                    ctx->active_code[i] = code;
                }
            }
        } else {
            if (ctx->key_state[i] != 0U) {
                uint32_t code = ctx->active_code[i] == KB_KEY_DROPPED ? 0U : ctx->active_code[i];
                (void)kb_execute_action_chain(ctx, layer, i, code, 0U);
            }
            ctx->key_state[i] = 0U;
            ctx->active_code[i] = 0U;
        }
    }

    kb_rebuild_hid_report(ctx);
    return 0;
}

int nlk_cap_keyboard_report_switches(struct nlk_keyboard_ctx *ctx, nlk_addr_t target_addr,
                                     uint8_t dst_cap, uint8_t module_id,
                                     const struct nlk_kb_switch_item *items, uint8_t count)
{
    uint8_t buf[sizeof(struct nlk_kb_switch_event_hdr) + 16U * sizeof(struct nlk_kb_switch_item)];
    struct nlk_kb_switch_event_hdr hdr;

    if (ctx == NULL || ctx->node == NULL || items == NULL || count == 0U || count > 16U) {
        return -EINVAL;
    }
    hdr.module_id = module_id;
    hdr.active_layer = ctx->active_layer;
    hdr.event_count = count;
    hdr.reserved = 0U;

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), items, count * sizeof(*items));

    return nlk_node_send_event(ctx->node, target_addr, dst_cap, NLK_KB_EVT_SWITCH_DELTA, buf,
                               (uint16_t)(sizeof(hdr) + count * sizeof(*items)), false);
}

int nlk_cap_keyboard_broadcast_light_beacon(struct nlk_keyboard_ctx *ctx,
                                            uint32_t master_frame_tick)
{
    struct nlk_kb_named_light_sync_beacon named;
    uint16_t len = sizeof(named.beacon);

    if (ctx == NULL || ctx->node == NULL) {
        return -EINVAL;
    }
    memset(&named, 0, sizeof(named));
    named.beacon.master_frame_tick = master_frame_tick;
    named.beacon.effect_id = ctx->light_cfg.effect_id;
    named.beacon.speed = ctx->light_cfg.speed;
    named.beacon.brightness = ctx->light_cfg.brightness;
    named.beacon.hue_offset = (uint8_t)(master_frame_tick & 0xFFU);
    ctx->last_light_beacon = named.beacon;
    {
        const struct nlk_kb_effect_entry *entry =
            nlk_cap_keyboard_find_light_effect(ctx, ctx->light_cfg.effect_id);
        if (entry != NULL) {
            /* The source is now an exact-length string in read-only storage. */
            strncpy(named.effect_name, nlk_kb_effect_name(entry), sizeof(named.effect_name) - 1U);
            len = sizeof(named);
        }
    }

    return nlk_node_send_event(ctx->node, NLK_ADDR_BROADCAST, NLK_CAP_ID_BROADCAST,
                               NLK_KB_EVT_LIGHT_SYNC_BEACON, &named, len, false);
}
