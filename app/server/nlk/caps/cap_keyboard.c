/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0100: Multi-Form-Factor Keyboard (60%/65%/75%/TKL/96%/100%/
 * Split/Ortho/Alice/Numpad/Macropad, Mechanical & Hall-Analog Switches,
 * 6KRO + NKRO, Cascaded Sub-Module Aggregation & Integrated Keyboard Lighting).
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

/* Standard HID modifier keycodes (0xE0..0xE7) & Layer shift codes (0xE8=FN, 0xE9=PN, 0xEA=MO3) */
#define NLK_KC_LCTRL    0xE0U
#define NLK_KC_RGUI     0xE7U
#define NLK_KC_FN       0xE8U
#define NLK_KC_PN       0xE9U
#define NLK_KC_MO3      0xEAU

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

static void kb_rebuild_hid_report(struct nlk_keyboard_ctx *ctx)
{
    uint8_t layer = 0U;
    uint8_t i;
    uint8_t key_slot = 0U;

    /* Check if FN / PN / MO3 layer shift key is held on base layer 0 */
    for (i = 0U; i < ctx->desc.key_count; i++) {
        if (ctx->key_state[i] != 0U) {
            uint8_t base_kc = ctx->keymap[0][i];
            if (base_kc == NLK_KC_MO3) {
                layer = 3U;
                break;
            }
            if (base_kc == NLK_KC_FN) {
                layer = 1U;
                break;
            }
            if (base_kc == NLK_KC_PN) {
                layer = 2U;
            }
        }
    }
    if (layer >= ctx->desc.layer_count) {
        layer = 0U;
    }
    ctx->active_layer = layer;

    memset(&ctx->last_report, 0, sizeof(ctx->last_report));
    memset(&ctx->last_nkro, 0, sizeof(ctx->last_nkro));
    ctx->last_report.report_id = 1U;
    ctx->last_nkro.report_id = 2U;

    for (i = 0U; i < ctx->desc.key_count; i++) {
        uint8_t kc;

        if (ctx->key_state[i] == 0U) {
            continue;
        }
        kc = ctx->keymap[layer][i];
        if (kc == 0U || kc == NLK_KC_FN || kc == NLK_KC_PN || kc == NLK_KC_MO3) {
            continue;
        }
        if (kc >= NLK_KC_LCTRL && kc <= NLK_KC_RGUI) {
            uint8_t mod_bit = (uint8_t)(1U << (kc - NLK_KC_LCTRL));
            ctx->last_report.modifiers |= mod_bit;
            ctx->last_nkro.modifiers |= mod_bit;
        } else {
            if (key_slot < sizeof(ctx->last_report.keys)) {
                ctx->last_report.keys[key_slot++] = kc;
            }
            if (kc < 128U) {
                ctx->last_nkro.bitmap[kc >> 3U] |= (uint8_t)(1U << (kc & 0x07U));
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

static int kb_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                       void *buf, uint16_t max_len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;

    (void)node;
    if (buf == NULL || max_len < sizeof(ctx->desc)) {
        return -ENOSPC;
    }
    memcpy(buf, &ctx->desc, sizeof(ctx->desc));
    return (int)sizeof(ctx->desc);
}

static int kb_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                     const struct nlk_pkt_hdr *hdr,
                     const void *req, uint16_t req_len,
                     void *resp, uint16_t *resp_len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_KB_OP_GET_DESC:
        rc = kb_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_KB_OP_GET_KEYMAP: {
        const struct nlk_kb_keymap_hdr *khdr = (const struct nlk_kb_keymap_hdr *)req;
        struct nlk_kb_keymap_hdr *out_hdr = (struct nlk_kb_keymap_hdr *)resp;
        struct nlk_kb_keymap_entry *entries;
        uint8_t i;

        if (req == NULL || req_len < sizeof(*khdr) || resp == NULL) {
            return -EINVAL;
        }
        if (khdr->layer >= ctx->desc.layer_count ||
            (uint32_t)khdr->start_index + khdr->count > ctx->desc.key_count) {
            return -EINVAL;
        }
        if (sizeof(*out_hdr) + (size_t)khdr->count * sizeof(*entries) > CONFIG_NLK_PACKET_MTU) {
            return -ENOSPC;
        }
        *out_hdr = *khdr;
        entries = (struct nlk_kb_keymap_entry *)((uint8_t *)resp + sizeof(*out_hdr));
        for (i = 0U; i < khdr->count; i++) {
            uint8_t idx = (uint8_t)(khdr->start_index + i);
            entries[i].key_code = ctx->keymap[khdr->layer][idx];
            entries[i].action = ctx->action_map[khdr->layer][idx];
        }
        *resp_len = (uint16_t)(sizeof(*out_hdr) + khdr->count * sizeof(*entries));
        return 0;
    }

    case NLK_KB_OP_SET_KEYMAP: {
        const struct nlk_kb_keymap_hdr *khdr = (const struct nlk_kb_keymap_hdr *)req;
        const struct nlk_kb_keymap_entry *entries;
        uint8_t i;

        if (req == NULL || req_len < sizeof(*khdr)) {
            return -EINVAL;
        }
        if (khdr->layer >= ctx->desc.layer_count ||
            (uint32_t)khdr->start_index + khdr->count > ctx->desc.key_count ||
            req_len < sizeof(*khdr) + (size_t)khdr->count * sizeof(*entries)) {
            return -EINVAL;
        }
        entries = (const struct nlk_kb_keymap_entry *)((const uint8_t *)req + sizeof(*khdr));
        for (i = 0U; i < khdr->count; i++) {
            uint8_t idx = (uint8_t)(khdr->start_index + i);
            ctx->keymap[khdr->layer][idx] = entries[i].key_code;
            ctx->action_map[khdr->layer][idx] = entries[i].action;
        }
        return 0;
    }

    case NLK_KB_OP_GET_SET_PARAM:
        if (req != NULL && req_len >= 1U) {
            ctx->desc.debounce_ms = ((const uint8_t *)req)[0];
        }
        if (resp != NULL) {
            ((uint8_t *)resp)[0] = ctx->desc.debounce_ms;
            *resp_len = 1U;
        }
        return 0;

    case NLK_KB_OP_GET_LIGHT_CFG:
        if (resp == NULL) {
            return -EINVAL;
        }
        memcpy(resp, &ctx->light_cfg, sizeof(ctx->light_cfg));
        *resp_len = sizeof(ctx->light_cfg);
        return 0;

    case NLK_KB_OP_SET_LIGHT_CFG:
        if (req == NULL || req_len < sizeof(ctx->light_cfg)) {
            return -EINVAL;
        }
        memcpy(&ctx->light_cfg, req, sizeof(ctx->light_cfg));
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
        }
        return 0;

    case NLK_KB_OP_SET_INDICATORS:
        if (req == NULL || req_len < 1U) {
            return -EINVAL;
        }
        ctx->indicator_leds = *(const uint8_t *)req;
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
        }
        return 0;

    case NLK_KB_OP_GET_ANALOG_CFG:
        if (resp == NULL) {
            return -EINVAL;
        }
        memcpy(resp, &ctx->analog_cfg, sizeof(ctx->analog_cfg));
        *resp_len = sizeof(ctx->analog_cfg);
        return 0;

    case NLK_KB_OP_SET_ANALOG_CFG:
        if (req == NULL || req_len < sizeof(ctx->analog_cfg)) {
            return -EINVAL;
        }
        memcpy(&ctx->analog_cfg, req, sizeof(ctx->analog_cfg));
        return 0;

    case NLK_KB_OP_GET_NKRO_REPORT:
        if (resp == NULL) {
            return -EINVAL;
        }
        memcpy(resp, &ctx->last_nkro, sizeof(ctx->last_nkro));
        *resp_len = sizeof(ctx->last_nkro);
        return 0;

    default:
        return -ENOTSUP;
    }
}

static int kb_on_event(struct nlk_node *node, struct nlk_capability *cap,
                       const struct nlk_pkt_hdr *hdr,
                       const void *evt, uint16_t evt_len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;

    (void)node;
    if (ctx == NULL || evt == NULL) {
        return -EINVAL;
    }

    if (hdr->cmd_op == NLK_KB_EVT_SWITCH_DELTA) {
        const struct nlk_kb_switch_event_hdr *ehdr =
            (const struct nlk_kb_switch_event_hdr *)evt;
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
                ctx->key_state[idx] = items[i].state;
                if (items[i].key_code != 0U && ctx->keymap[0][idx] == 0U) {
                    ctx->keymap[0][idx] = items[i].key_code;
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
        ctx->light_cfg.effect_id = ctx->last_light_beacon.effect_id;
        ctx->light_cfg.speed = ctx->last_light_beacon.speed;
        ctx->light_cfg.brightness = ctx->last_light_beacon.brightness;
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, &ctx->last_light_beacon,
                          ctx->indicator_leds, ctx->light_priv);
        }
        return 0;
    }

    return -ENOTSUP;
}

static int kb_on_stream(struct nlk_node *node, struct nlk_capability *cap,
                        const struct nlk_pkt_hdr *hdr,
                        const void *data, uint16_t len)
{
    struct nlk_keyboard_ctx *ctx = (struct nlk_keyboard_ctx *)cap->priv;
    const struct nlk_kb_light_frame_hdr *fhdr =
        (const struct nlk_kb_light_frame_hdr *)data;

    (void)node;
    if (ctx == NULL || data == NULL ||
        hdr->cmd_op != NLK_KB_STREAM_LIGHT_FRAME || len < sizeof(*fhdr)) {
        return -EINVAL;
    }
    if (ctx->light_frame_cb != NULL) {
        ctx->light_frame_cb(fhdr->start_led, fhdr->led_count,
                            (const uint8_t *)data + sizeof(*fhdr),
                            (uint16_t)(len - sizeof(*fhdr)), ctx->light_priv);
    }
    return 0;
}

static const struct nlk_cap_ops g_kb_ops = {
    .get_desc  = kb_get_desc,
    .on_rpc    = kb_on_rpc,
    .on_event  = kb_on_event,
    .on_stream = kb_on_stream,
};

int nlk_cap_keyboard_init(struct nlk_node *node, struct nlk_keyboard_ctx *ctx,
                          uint8_t rows, uint8_t cols, uint8_t key_count,
                          nlk_kb_report_cb_t report_cb, void *priv)
{
    if (node == NULL || ctx == NULL || key_count > NLK_KB_MAX_KEYS) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->desc.form_factor = kb_infer_form_factor(key_count);
    ctx->desc.switch_type = NLK_KB_SW_MECH;
    ctx->desc.kb_flags = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO |
                         NLK_KB_FLAG_MEDIA_KEYS | NLK_KB_FLAG_MODULAR_HUB |
                         NLK_KB_FLAG_LIGHT_RGB_KEY | NLK_KB_FLAG_LIGHT_INDICATORS;
    ctx->desc.matrix_rows = rows;
    ctx->desc.matrix_cols = cols;
    ctx->desc.key_count = key_count;
    ctx->desc.layer_count = NLK_KB_MAX_LAYERS;
    ctx->desc.debounce_ms = 5U;
    ctx->desc.led_matrix_rows = rows;
    ctx->desc.led_matrix_cols = cols;
    ctx->desc.underglow_leds = 0U;
    ctx->desc.led_count = key_count;
    ctx->desc.poll_rate_hz = 1000U;

    ctx->analog_cfg.actuation_01mm = 20U;
    ctx->analog_cfg.rapid_trigger_en = 0U;
    ctx->analog_cfg.rt_press_01mm = 3U;
    ctx->analog_cfg.rt_release_01mm = 3U;
    ctx->analog_cfg.bottom_deadzone = 2U;
    ctx->analog_cfg.top_deadzone = 2U;

    ctx->light_cfg.enable = 1U;
    ctx->light_cfg.zone_mask = NLK_KB_LIGHT_ZONE_ALL;
    ctx->light_cfg.effect_id = NLK_KB_LIGHT_EFF_GRADIENT_WAVE;
    ctx->light_cfg.brightness = 200U;
    ctx->light_cfg.speed = 128U;

    ctx->report_cb = report_cb;
    ctx->report_priv = priv;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_KEYBOARD;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_EVENT | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = ctx->desc.form_factor;
    ctx->cap.desc_size = sizeof(struct nlk_kb_desc);
    ctx->cap.name = "keyboard";
    ctx->cap.ops = &g_kb_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_keyboard_configure(struct nlk_keyboard_ctx *ctx,
                               const struct nlk_kb_desc *desc)
{
    if (ctx == NULL || desc == NULL ||
        desc->key_count == 0U || desc->key_count > NLK_KB_MAX_KEYS ||
        desc->layer_count == 0U || desc->layer_count > NLK_KB_MAX_LAYERS) {
        return -EINVAL;
    }
    ctx->desc = *desc;
    ctx->cap.sub_class = desc->form_factor;
    return 0;
}

int nlk_cap_keyboard_set_light_cb(struct nlk_keyboard_ctx *ctx,
                                  nlk_kb_light_cb_t light_cb,
                                  nlk_kb_light_frame_cb_t frame_cb,
                                  void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->light_cb = light_cb;
    ctx->light_frame_cb = frame_cb;
    ctx->light_priv = priv;
    return 0;
}

int nlk_cap_keyboard_set_module_offset(struct nlk_keyboard_ctx *ctx,
                                       uint8_t module_id, uint8_t key_offset)
{
    if (ctx == NULL || module_id >= 8U || key_offset >= NLK_KB_MAX_KEYS) {
        return -EINVAL;
    }
    ctx->module_key_offset[module_id] = key_offset;
    return 0;
}

int nlk_cap_keyboard_load_layout(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                 const uint8_t *key_codes, const uint8_t *actions,
                                 uint8_t count)
{
    if (ctx == NULL || layer >= ctx->desc.layer_count || count > ctx->desc.key_count) {
        return -EINVAL;
    }
    if (key_codes != NULL) {
        memcpy(ctx->keymap[layer], key_codes, count);
    }
    if (actions != NULL) {
        memcpy(ctx->action_map[layer], actions, count);
    }
    return 0;
}

int nlk_cap_keyboard_report_switches(struct nlk_keyboard_ctx *ctx,
                                     nlk_addr_t target_addr, uint8_t dst_cap,
                                     uint8_t module_id,
                                     const struct nlk_kb_switch_item *items,
                                     uint8_t count)
{
    uint8_t buf[ sizeof(struct nlk_kb_switch_event_hdr) +
                 16U * sizeof(struct nlk_kb_switch_item) ];
    struct nlk_kb_switch_event_hdr hdr;

    if (ctx == NULL || ctx->node == NULL || items == NULL ||
        count == 0U || count > 16U) {
        return -EINVAL;
    }
    hdr.module_id = module_id;
    hdr.active_layer = ctx->active_layer;
    hdr.event_count = count;
    hdr.reserved = 0U;

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), items, count * sizeof(*items));

    return nlk_node_send_event(ctx->node, target_addr, dst_cap,
                               NLK_KB_EVT_SWITCH_DELTA, buf,
                               (uint16_t)(sizeof(hdr) + count * sizeof(*items)),
                               false);
}

int nlk_cap_keyboard_broadcast_light_beacon(struct nlk_keyboard_ctx *ctx,
                                            uint32_t master_frame_tick)
{
    struct nlk_kb_light_sync_beacon beacon;

    if (ctx == NULL || ctx->node == NULL) {
        return -EINVAL;
    }
    beacon.master_frame_tick = master_frame_tick;
    beacon.effect_id = ctx->light_cfg.effect_id;
    beacon.speed = ctx->light_cfg.speed;
    beacon.brightness = ctx->light_cfg.brightness;
    beacon.hue_offset = (uint8_t)(master_frame_tick & 0xFFU);
    ctx->last_light_beacon = beacon;

    return nlk_node_send_event(ctx->node, NLK_ADDR_BROADCAST,
                               NLK_CAP_ID_BROADCAST,
                               NLK_KB_EVT_LIGHT_SYNC_BEACON,
                               &beacon, sizeof(beacon), false);
}
