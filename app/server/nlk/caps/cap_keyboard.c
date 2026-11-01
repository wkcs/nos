/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0100: Multi-Form-Factor Keyboard (60%/65%/75%/TKL/96%/100%/
 * Split/Ortho/Alice/Numpad/Macropad, Mechanical & Hall-Analog Switches,
 * 6KRO + NKRO, Cascaded Sub-Module Aggregation & Integrated Keyboard Lighting).
 */

#include <nos/nlk_caps.h>
#include <kernel/mm.h>
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

static uint8_t kb_resolve_active_layer(const struct nlk_keyboard_ctx *ctx)
{
    uint8_t layer = ctx->default_layer;
    uint8_t i;

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
            if (base_kc == NLK_KC_PN && layer == 0U) {
                layer = 2U;
            }
        }
    }
    if (layer >= ctx->desc.layer_count) {
        layer = 0U;
    }
    return layer;
}

static void kb_rebuild_hid_report(struct nlk_keyboard_ctx *ctx)
{
    uint8_t prev_keys[6];
    uint8_t data_bck[6] = { 0 };
    uint8_t data_new[6] = { 0 };
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

    for (i = 0U; i < ctx->desc.key_count; i++) {
        uint8_t kc;
        bool is_old;

        if (ctx->key_state[i] == 0U) {
            ctx->active_code[i] = 0U;
            continue;
        }
        if (ctx->action_chains[layer][i].count > 0U) {
            kc = ctx->active_code[i];
        } else if (layer > 0U && ctx->keymap[layer][i] != 0U) {
            kc = ctx->keymap[layer][i];
            ctx->active_code[i] = kc;
        } else if (ctx->active_code[i] != 0U) {
            kc = ctx->active_code[i];
        } else {
            kc = ctx->keymap[layer][i];
            ctx->active_code[i] = kc;
        }

        if (kc == 0U || kc == NLK_KC_FN || kc == NLK_KC_PN || kc == NLK_KC_MO3) {
            continue;
        }
        if (kc >= NLK_KC_LCTRL && kc <= NLK_KC_RGUI) {
            uint8_t mod_bit = (uint8_t)(1U << (kc - NLK_KC_LCTRL));
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
        if (kc == 0U) continue;
        if (kc < 128U) ctx->last_nkro.bitmap[kc >> 3U] |= (uint8_t)(1U << (kc & 7U));
        for (n = 0U; n < 6U; n++) {
            if (ctx->last_report.keys[n] == kc) break;
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

static uint8_t kb_find_action_id(const struct nlk_keyboard_ctx *ctx, const char *name)
{
    uint8_t i;
    if (ctx == NULL || name == NULL || name[0] == '\0') {
        return 0U;
    }
    for (i = 0U; i < ctx->action_count && i < NLK_KB_MAX_ACTIONS; i++) {
        if (strncmp(ctx->action_descs[i].name, name, NLK_KB_ACTION_NAME_MAX) == 0) {
            return ctx->action_descs[i].action_id;
        }
    }
    return 0U;
}

static const char *kb_find_action_name(const struct nlk_keyboard_ctx *ctx, uint8_t action_id)
{
    uint8_t i;
    if (ctx == NULL || action_id == 0U) {
        return "";
    }
    for (i = 0U; i < ctx->action_count && i < NLK_KB_MAX_ACTIONS; i++) {
        if (ctx->action_descs[i].action_id == action_id) {
            return ctx->action_descs[i].name;
        }
    }
    return "";
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
            (type != NLK_KB_MACRO_ACT_DELAY_MS && (kc == 0U || kc >= NLK_KC_FN))) {
            return false;
        }
    }
    return true;
}

static void kb_play_macro(struct nlk_keyboard_ctx *ctx, uint8_t macro_id);
static void kb_record_event(struct nlk_keyboard_ctx *ctx, uint8_t type, uint8_t key_code);
static uint8_t kb_execute_action_chain(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                       uint8_t key_index, uint8_t initial_code,
                                       uint8_t pressed);

static void kb_sync_active_config(struct nlk_keyboard_ctx *ctx)
{
    uint16_t resp_len = 0U;
    if (ctx != NULL && ctx->config_files_cb != NULL) {
        (void)ctx->config_files_cb(0U, NULL, 0U, NULL, &resp_len,
                                   ctx->config_files_priv);
    }
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
    case NLK_KB_OP_CONFIG_FILES_INFO:
    case NLK_KB_OP_CONFIG_FILES_LIST:
    case NLK_KB_OP_CONFIG_FILES_SAVE:
    case NLK_KB_OP_CONFIG_FILES_LOAD:
    case NLK_KB_OP_CONFIG_FILES_DELETE:
        if (ctx->config_files_cb == NULL) {
            return -ENOTSUP;
        }
        return ctx->config_files_cb(hdr->cmd_op, req, req_len, resp, resp_len,
                                    ctx->config_files_priv);
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
            entries[i].action = (ctx->action_chains[khdr->layer][idx].count > 0U) ?
                                ctx->action_chains[khdr->layer][idx].action_ids[0] : 0U;
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
            if (entries[i].action != 0U) {
                ctx->action_chains[khdr->layer][idx].count = 1U;
                ctx->action_chains[khdr->layer][idx].action_ids[0] = entries[i].action;
            } else {
                ctx->action_chains[khdr->layer][idx].count = 0U;
            }
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_GET_SET_PARAM:
        if (req != NULL && req_len >= 1U) {
            ctx->desc.debounce_ms = ((const uint8_t *)req)[0];
            kb_sync_active_config(ctx);
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
        if (ctx->light_effect_count != 0U &&
            ((const struct nlk_kb_light_cfg *)req)->effect_id >= ctx->light_effect_count) {
            return -EINVAL;
        }
        memcpy(&ctx->light_cfg, req, sizeof(ctx->light_cfg));
        if (ctx->light_zone_count > 0U) {
            struct nlk_kb_zone_light_cfg *zone = &ctx->zone_cfgs[0];
            zone->enable = ctx->light_cfg.enable;
            zone->effect_id = ctx->light_cfg.effect_id;
            zone->brightness = ctx->light_cfg.brightness;
            zone->speed = ctx->light_cfg.speed;
            zone->color_r = ctx->light_cfg.color_r;
            zone->color_g = ctx->light_cfg.color_g;
            zone->color_b = ctx->light_cfg.color_b;
        }
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
        }
        kb_sync_active_config(ctx);
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
        kb_sync_active_config(ctx);
        return 0;

    case NLK_KB_OP_GET_NKRO_REPORT:
        if (resp == NULL) {
            return -EINVAL;
        }
        memcpy(resp, &ctx->last_nkro, sizeof(ctx->last_nkro));
        *resp_len = sizeof(ctx->last_nkro);
        return 0;

    case NLK_KB_OP_GET_LAYOUT_LIST: {
        struct nlk_kb_layout_list_resp *lresp = (struct nlk_kb_layout_list_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(lresp, 0, sizeof(*lresp));
        lresp->layout_count = ctx->layout_count;
        lresp->active_layout_id = ctx->active_layout_id;
        if (ctx->layout_count > 0U) {
            uint8_t cnt = (ctx->layout_count > NLK_KB_MAX_LAYOUTS) ?
                          NLK_KB_MAX_LAYOUTS : ctx->layout_count;
            memcpy(lresp->layouts, ctx->layouts, sizeof(struct nlk_kb_layout_info) * cnt);
        }
        *resp_len = sizeof(*lresp);
        return 0;
    }

    case NLK_KB_OP_SET_LAYOUT: {
        const struct nlk_kb_set_layout_req *lreq = (const struct nlk_kb_set_layout_req *)req;
        uint8_t i;
        bool found = false;

        if (req == NULL || req_len < sizeof(*lreq)) {
            return -EINVAL;
        }
        if (ctx->layout_count == 0U) {
            ctx->active_layout_id = lreq->layout_id;
            ctx->params.active_layout_id = lreq->layout_id;
            if (ctx->layout_cb != NULL) {
                (void)ctx->layout_cb(lreq->layout_id, ctx->layout_priv);
            }
            kb_sync_active_config(ctx);
            return 0;
        }
        for (i = 0U; i < ctx->layout_count && i < NLK_KB_MAX_LAYOUTS; i++) {
            if (ctx->layouts[i].layout_id == lreq->layout_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return -EINVAL;
        }
        ctx->active_layout_id = lreq->layout_id;
        ctx->params.active_layout_id = lreq->layout_id;
        if (ctx->layout_cb != NULL) {
            (void)ctx->layout_cb(lreq->layout_id, ctx->layout_priv);
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_GET_LIGHT_EFFECTS: {
        struct nlk_kb_light_effect_list_resp *effects = resp;
        const struct nlk_kb_effect_entry *entry;
        uint16_t i;
        uint16_t count = ctx->light_effect_count;
        if (effects == NULL) {
            return -EINVAL;
        }
        if (count > NLK_KB_LIGHT_EFFECTS_PER_PAGE) {
            count = NLK_KB_LIGHT_EFFECTS_PER_PAGE;
        }
        memset(effects, 0, sizeof(*effects));
        effects->effect_count = (uint8_t)count;
        entry = ctx->light_effects;
        for (i = 0U; i < count && entry != NULL; i++, entry = entry->next) {
            effects->effects[i] = entry->info;
        }
        *resp_len = sizeof(*effects);
        return 0;
    }

    case NLK_KB_OP_GET_LIGHT_EFFECTS_PAGE: {
        const struct nlk_kb_light_effect_page_req *page_req = req;
        struct nlk_kb_light_effect_page_resp *page = resp;
        const struct nlk_kb_effect_entry *entry;
        uint16_t i;

        if (page_req == NULL || req_len < sizeof(*page_req) || page == NULL ||
            page_req->start_index > ctx->light_effect_count) {
            return -EINVAL;
        }
        memset(page, 0, sizeof(*page));
        page->total_count = ctx->light_effect_count;
        entry = ctx->light_effects;
        for (i = 0U; i < page_req->start_index && entry != NULL; i++) {
            entry = entry->next;
        }
        while (entry != NULL && page->count < NLK_KB_LIGHT_EFFECTS_PER_PAGE) {
            page->effects[page->count++] = entry->info;
            entry = entry->next;
        }
        *resp_len = sizeof(*page);
        return 0;
    }

    case NLK_KB_OP_GET_LIGHT_ZONES: {
        struct nlk_kb_light_zones_resp *zresp = (struct nlk_kb_light_zones_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(zresp, 0, sizeof(*zresp));
        zresp->zone_count = ctx->light_zone_count;
        zresp->global_enable = ctx->light_cfg.enable;
        if (ctx->light_zone_count > 0U) {
            uint8_t cnt = (ctx->light_zone_count > NLK_KB_MAX_LIGHT_ZONES) ?
                          NLK_KB_MAX_LIGHT_ZONES : ctx->light_zone_count;
            memcpy(zresp->zones, ctx->light_zones, sizeof(struct nlk_kb_light_zone_desc) * cnt);
        }
        *resp_len = sizeof(*zresp);
        return 0;
    }

    case NLK_KB_OP_GET_ZONE_LIGHT_CFG: {
        uint8_t zid;
        if (req == NULL || req_len < 1U || resp == NULL) {
            return -EINVAL;
        }
        zid = *(const uint8_t *)req;
        if (zid >= NLK_KB_MAX_LIGHT_ZONES) {
            return -EINVAL;
        }
        memcpy(resp, &ctx->zone_cfgs[zid], sizeof(struct nlk_kb_zone_light_cfg));
        *resp_len = sizeof(struct nlk_kb_zone_light_cfg);
        return 0;
    }

    case NLK_KB_OP_SET_ZONE_LIGHT_CFG: {
        const struct nlk_kb_zone_light_cfg *zcfg = (const struct nlk_kb_zone_light_cfg *)req;
        if (req == NULL || req_len < sizeof(*zcfg)) {
            return -EINVAL;
        }
        if (zcfg->zone_id >= NLK_KB_MAX_LIGHT_ZONES) {
            return -EINVAL;
        }
        if (ctx->light_effect_count != 0U && zcfg->effect_id >= ctx->light_effect_count) {
            return -EINVAL;
        }
        ctx->zone_cfgs[zcfg->zone_id] = *zcfg;
        if (zcfg->zone_id == 0U) {
            ctx->light_cfg.enable = zcfg->enable;
            ctx->light_cfg.effect_id = zcfg->effect_id;
            ctx->light_cfg.brightness = zcfg->brightness;
            ctx->light_cfg.speed = zcfg->speed;
            ctx->light_cfg.color_r = zcfg->color_r;
            ctx->light_cfg.color_g = zcfg->color_g;
            ctx->light_cfg.color_b = zcfg->color_b;
            if (ctx->light_cb != NULL) {
                ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
            }
        }
        if (ctx->zone_light_cb != NULL) {
            ctx->zone_light_cb(zcfg->zone_id, zcfg, ctx->zone_light_priv);
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_GET_LAYERS: {
        struct nlk_kb_layers_resp *lay_resp = (struct nlk_kb_layers_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(lay_resp, 0, sizeof(*lay_resp));
        lay_resp->layer_count = ctx->desc.layer_count;
        lay_resp->active_layer = ctx->active_layer;
        lay_resp->default_layer = ctx->default_layer;
        memcpy(lay_resp->layers, ctx->layers, sizeof(struct nlk_kb_layer_desc) * ctx->desc.layer_count);
        *resp_len = sizeof(*lay_resp);
        return 0;
    }

    case NLK_KB_OP_SET_ACTIVE_LAYER: {
        const struct nlk_kb_set_active_layer_req *areq =
            (const struct nlk_kb_set_active_layer_req *)req;
        if (req == NULL || req_len < sizeof(*areq)) {
            return -EINVAL;
        }
        if (areq->active_layer >= ctx->desc.layer_count) {
            return -EINVAL;
        }
        ctx->default_layer = areq->active_layer;
        ctx->active_layer = areq->active_layer;
        kb_rebuild_hid_report(ctx);
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_GET_MACRO_LIST: {
        struct nlk_kb_macro_list_resp *mresp = (struct nlk_kb_macro_list_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(mresp, 0, sizeof(*mresp));
        mresp->macro_count = NLK_KB_MAX_MACROS;
        mresp->recording_macro_id = ctx->recording_macro_id;
        memcpy(mresp->macros, ctx->macro_info, sizeof(struct nlk_kb_macro_info) * NLK_KB_MAX_MACROS);
        *resp_len = sizeof(*mresp);
        return 0;
    }

    case NLK_KB_OP_GET_MACRO: {
        uint8_t mid;
        struct nlk_kb_macro_cfg_hdr *mhdr = (struct nlk_kb_macro_cfg_hdr *)resp;
        uint8_t steps_cnt;
        size_t total_sz;

        if (req == NULL || req_len < 1U || resp == NULL) {
            return -EINVAL;
        }
        mid = *(const uint8_t *)req;
        if (mid >= NLK_KB_MAX_MACROS) {
            return -EINVAL;
        }
        steps_cnt = ctx->macro_info[mid].step_count;
        if (steps_cnt > NLK_KB_MAX_MACRO_STEPS) {
            steps_cnt = NLK_KB_MAX_MACRO_STEPS;
        }
        total_sz = sizeof(*mhdr) + (size_t)steps_cnt * sizeof(struct nlk_kb_macro_step);
        if (total_sz > CONFIG_NLK_PACKET_MTU) {
            return -ENOSPC;
        }
        memset(mhdr, 0, sizeof(*mhdr));
        mhdr->macro_id = mid;
        mhdr->step_count = steps_cnt;
        mhdr->repeat_count = ctx->macro_info[mid].repeat_count;
        memcpy(mhdr->name, ctx->macro_info[mid].name, NLK_KB_MACRO_NAME_MAX);
        if (steps_cnt > 0U) {
            memcpy((uint8_t *)resp + sizeof(*mhdr),
                   ctx->macro_steps[mid],
                   (size_t)steps_cnt * sizeof(struct nlk_kb_macro_step));
        }
        *resp_len = (uint16_t)total_sz;
        return 0;
    }

    case NLK_KB_OP_SET_MACRO: {
        const struct nlk_kb_macro_cfg_hdr *mhdr = (const struct nlk_kb_macro_cfg_hdr *)req;
        const struct nlk_kb_macro_step *steps;
        uint8_t mid;

        if (req == NULL || req_len < sizeof(*mhdr)) {
            return -EINVAL;
        }
        mid = mhdr->macro_id;
        if (mid >= NLK_KB_MAX_MACROS || mhdr->step_count > NLK_KB_MAX_MACRO_STEPS) {
            return -EINVAL;
        }
        if (req_len < sizeof(*mhdr) + (size_t)mhdr->step_count * sizeof(struct nlk_kb_macro_step)) {
            return -EINVAL;
        }
        steps = (const struct nlk_kb_macro_step *)((const uint8_t *)req + sizeof(*mhdr));
        if (!kb_macro_steps_valid(steps, mhdr->step_count)) {
            return -EINVAL;
        }
        if (ctx->macro_playing != 0U && ctx->macro_play_id == mid) {
            (void)nlk_cap_keyboard_macro_cancel(ctx);
        }
        ctx->macro_info[mid].macro_id = mid;
        ctx->macro_info[mid].step_count = mhdr->step_count;
        ctx->macro_info[mid].repeat_count = (mhdr->repeat_count == 0U) ? 1U : mhdr->repeat_count;
        memcpy(ctx->macro_info[mid].name, mhdr->name, NLK_KB_MACRO_NAME_MAX);
        if (mhdr->step_count > 0U) {
            memcpy(ctx->macro_steps[mid], steps,
                   (size_t)mhdr->step_count * sizeof(struct nlk_kb_macro_step));
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_RECORD_MACRO: {
        const struct nlk_kb_macro_record_req *rreq =
            (const struct nlk_kb_macro_record_req *)req;
        if (req == NULL || req_len < sizeof(*rreq)) {
            return -EINVAL;
        }
        if (rreq->macro_id >= NLK_KB_MAX_MACROS) {
            return -EINVAL;
        }
        if (rreq->record_cmd == 1U) {
            /* Start recording */
            if (ctx->macro_playing != 0U) {
                (void)nlk_cap_keyboard_macro_cancel(ctx);
            }
            ctx->recording_macro_id = rreq->macro_id;
            ctx->macro_info[rreq->macro_id].macro_id = rreq->macro_id;
            ctx->macro_info[rreq->macro_id].step_count = 0U;
            if (ctx->macro_info[rreq->macro_id].name[0] == '\0') {
                memcpy(ctx->macro_info[rreq->macro_id].name, "Recorded", 9);
            }
            ctx->macro_info[rreq->macro_id].flags = 0U;
            ctx->macro_info[rreq->macro_id].repeat_count = 1U;
            ctx->last_rec_tick = ctx->macro_clock_ms;
        } else if (rreq->record_cmd == 0U) {
            /* Stop recording */
            ctx->recording_macro_id = 0xFFU;
            kb_sync_active_config(ctx);
        } else if (rreq->record_cmd == 2U) {
            /* Clear */
            if (ctx->macro_playing != 0U && ctx->macro_play_id == rreq->macro_id) {
                (void)nlk_cap_keyboard_macro_cancel(ctx);
            }
            ctx->macro_info[rreq->macro_id].step_count = 0U;
            memset(ctx->macro_steps[rreq->macro_id], 0, sizeof(ctx->macro_steps[rreq->macro_id]));
            if (ctx->recording_macro_id == rreq->macro_id) {
                ctx->recording_macro_id = 0xFFU;
            }
            kb_sync_active_config(ctx);
        } else {
            return -EINVAL;
        }
        return 0;
    }

    case NLK_KB_OP_MACRO_CONTROL: {
        const struct nlk_kb_macro_control_req *creq =
            (const struct nlk_kb_macro_control_req *)req;
        if (req == NULL || req_len < sizeof(*creq)) {
            return -EINVAL;
        }
        if (creq->command == NLK_KB_MACRO_STOP) {
            return nlk_cap_keyboard_macro_cancel(ctx);
        }
        if (creq->command == NLK_KB_MACRO_PLAY) {
            return nlk_cap_keyboard_macro_start(ctx, creq->macro_id, ctx->macro_clock_ms);
        }
        return -EINVAL;
    }

    case NLK_KB_OP_MACRO_STATUS: {
        struct nlk_kb_macro_status_resp *sresp =
            (struct nlk_kb_macro_status_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(sresp, 0, sizeof(*sresp));
        sresp->playing = ctx->macro_playing;
        sresp->macro_id = ctx->macro_play_id;
        sresp->step_index = ctx->macro_play_index;
        sresp->repeats_left = ctx->macro_repeats_left;
        sresp->recording_macro_id = ctx->recording_macro_id;
        *resp_len = sizeof(*sresp);
        return 0;
    }

    case NLK_KB_OP_GET_ACTIONS: {
        struct nlk_kb_action_list_resp *aresp = (struct nlk_kb_action_list_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(aresp, 0, sizeof(*aresp));
        aresp->action_count = ctx->action_count;
        if (ctx->action_count > 0U) {
            uint8_t cnt = (ctx->action_count > NLK_KB_MAX_ACTIONS) ?
                          NLK_KB_MAX_ACTIONS : ctx->action_count;
            memcpy(aresp->actions, ctx->action_descs, sizeof(struct nlk_kb_action_desc) * cnt);
        }
        *resp_len = sizeof(*aresp);
        return 0;
    }

    case NLK_KB_OP_GET_PARAMS: {
        struct nlk_kb_params *presp = (struct nlk_kb_params *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        ctx->params.debounce_ms = ctx->desc.debounce_ms;
        ctx->params.poll_rate_hz = ctx->desc.poll_rate_hz;
        ctx->params.active_layout_id = ctx->active_layout_id;
        ctx->params.default_layer = ctx->default_layer;
        *presp = ctx->params;
        *resp_len = sizeof(*presp);
        return 0;
    }

    case NLK_KB_OP_SET_PARAMS: {
        const struct nlk_kb_params *preq = (const struct nlk_kb_params *)req;
        if (req == NULL || req_len < sizeof(*preq)) {
            return -EINVAL;
        }
        ctx->params = *preq;
        if (ctx->params.debounce_ms > 0U) {
            ctx->desc.debounce_ms = ctx->params.debounce_ms;
        }
        if (ctx->params.poll_rate_hz > 0U) {
            ctx->desc.poll_rate_hz = ctx->params.poll_rate_hz;
        }
        if (ctx->params.nkro_enable != 0U) {
            ctx->desc.kb_flags |= NLK_KB_FLAG_NKRO;
        } else {
            ctx->desc.kb_flags &= (uint16_t)~NLK_KB_FLAG_NKRO;
        }
        if (ctx->param_cb != NULL) {
            (void)ctx->param_cb(&ctx->params, ctx->param_priv);
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_RESET_PARAMS: {
        ctx->params = ctx->default_params;
        ctx->desc.debounce_ms = ctx->params.debounce_ms;
        ctx->desc.poll_rate_hz = ctx->params.poll_rate_hz;
        if (ctx->param_cb != NULL) {
            (void)ctx->param_cb(&ctx->params, ctx->param_priv);
        }
        kb_sync_active_config(ctx);
        return 0;
    }

    case NLK_KB_OP_GET_PROFILE_INFO: {
        struct nlk_kb_profile_info_resp *presp = (struct nlk_kb_profile_info_resp *)resp;
        if (resp == NULL) {
            return -EINVAL;
        }
        memset(presp, 0, sizeof(*presp));
        presp->slot_count = NLK_KB_MAX_PROFILE_SLOTS;
        presp->active_slot = ctx->active_profile_slot;
        presp->saved_slots_mask = ctx->saved_profiles_mask;
        if (ctx->profile_info_cb != NULL) {
            (void)ctx->profile_info_cb(&presp->active_slot, &presp->saved_slots_mask,
                                       ctx->profile_priv);
            ctx->active_profile_slot = presp->active_slot;
            ctx->saved_profiles_mask = presp->saved_slots_mask;
        }
        *resp_len = sizeof(*presp);
        return 0;
    }

    case NLK_KB_OP_SAVE_PROFILE: {
        const struct nlk_kb_profile_slot_req *sreq =
            (const struct nlk_kb_profile_slot_req *)req;
        uint8_t slot;
        if (req == NULL || req_len < sizeof(*sreq)) {
            return -EINVAL;
        }
        slot = sreq->slot_id;
        if (slot >= NLK_KB_MAX_PROFILE_SLOTS) {
            return -EINVAL;
        }
        if (ctx->profile_save_cb != NULL) {
            int ret = ctx->profile_save_cb(slot, ctx->profile_priv);
            if (ret != 0) {
                return ret;
            }
            ctx->saved_profiles_mask |= (uint8_t)(1U << slot);
        }
        return 0;
    }

    case NLK_KB_OP_LOAD_PROFILE: {
        const struct nlk_kb_profile_slot_req *sreq =
            (const struct nlk_kb_profile_slot_req *)req;
        uint8_t slot;
        if (req == NULL || req_len < sizeof(*sreq)) {
            return -EINVAL;
        }
        slot = sreq->slot_id;
        if (slot >= NLK_KB_MAX_PROFILE_SLOTS) {
            return -EINVAL;
        }
        if (ctx->profile_load_cb != NULL) {
            int ret = ctx->profile_load_cb(slot, ctx->profile_priv);
            if (ret != 0) {
                return ret;
            }
            ctx->active_profile_slot = slot;
        }
        return 0;
    }

    case NLK_KB_OP_BIND_ACTION: {
        const struct nlk_kb_bind_action_req *breq =
            (const struct nlk_kb_bind_action_req *)req;
        if (req == NULL || req_len < sizeof(*breq)) {
            return -EINVAL;
        }
        rc = nlk_cap_keyboard_bind_key_action(ctx, breq->layer, breq->key_index,
                                              breq->action_name, breq->append != 0U);
        if (rc == 0) {
            kb_sync_active_config(ctx);
        }
        return rc;
    }

    case NLK_KB_OP_UNBIND_ACTION: {
        const struct nlk_kb_unbind_action_req *ureq =
            (const struct nlk_kb_unbind_action_req *)req;
        if (req == NULL || req_len < sizeof(*ureq)) {
            return -EINVAL;
        }
        rc = nlk_cap_keyboard_unbind_key_action(ctx, ureq->layer, ureq->key_index,
                                                ureq->action_name);
        if (rc == 0) {
            kb_sync_active_config(ctx);
        }
        return rc;
    }

    case NLK_KB_OP_GET_KEY_ACTIONS: {
        const struct nlk_kb_get_key_actions_req *greq =
            (const struct nlk_kb_get_key_actions_req *)req;
        struct nlk_kb_key_actions_resp *kresp =
            (struct nlk_kb_key_actions_resp *)resp;
        uint8_t i;
        const struct nlk_kb_key_action_chain *chain;

        if (req == NULL || req_len < sizeof(*greq) || resp == NULL) {
            return -EINVAL;
        }
        if (greq->layer >= ctx->desc.layer_count || greq->key_index >= ctx->desc.key_count) {
            return -EINVAL;
        }
        memset(kresp, 0, sizeof(*kresp));
        chain = &ctx->action_chains[greq->layer][greq->key_index];
        kresp->action_count = chain->count;
        for (i = 0U; i < chain->count && i < NLK_KB_MAX_KEY_ACTIONS; i++) {
            const char *aname = kb_find_action_name(ctx, chain->action_ids[i]);
            strncpy(kresp->action_names[i], aname, NLK_KB_ACTION_NAME_MAX - 1);
            kresp->action_names[i][NLK_KB_ACTION_NAME_MAX - 1] = '\0';
        }
        *resp_len = sizeof(*kresp);
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

int nlk_cap_keyboard_set_config_files_cb(struct nlk_keyboard_ctx *ctx,
                                        nlk_kb_config_files_cb_t cb, void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->config_files_cb = cb;
    ctx->config_files_priv = priv;
    return 0;
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
                uint8_t old_st = ctx->key_state[idx];
                ctx->key_state[idx] = items[i].state;
                if (items[i].state == 0U) {
                    if (old_st != 0U) {
                        (void)kb_execute_action_chain(ctx, ehdr->active_layer, (uint8_t)idx,
                                                      ctx->active_code[idx], 0U);
                        if (ctx->recording_macro_id < NLK_KB_MAX_MACROS) {
                            kb_record_event(ctx, NLK_KB_MACRO_ACT_KEY_UP, ctx->active_code[idx]);
                        }
                    }
                    ctx->active_code[idx] = 0U;
                } else {
                    if (old_st == 0U) {
                        uint8_t final_kc = kb_execute_action_chain(ctx, ehdr->active_layer, (uint8_t)idx,
                                                                   items[i].key_code, 1U);
                        ctx->active_code[idx] = final_kc;
                        if (ctx->recording_macro_id < NLK_KB_MAX_MACROS) {
                            kb_record_event(ctx, NLK_KB_MACRO_ACT_KEY_DOWN, final_kc);
                        }
                    }
                }
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
        if (ctx->light_effect_count == 0U) {
            ctx->light_cfg.effect_id = ctx->last_light_beacon.effect_id;
        } else if (evt_len >= sizeof(struct nlk_kb_named_light_sync_beacon)) {
            const struct nlk_kb_named_light_sync_beacon *named = evt;
            const struct nlk_kb_effect_entry *entry;
            for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
                if (strncmp(named->effect_name, entry->info.name,
                            NLK_KB_LIGHT_EFFECT_NAME_MAX) == 0) {
                    ctx->light_cfg.effect_id = entry->info.effect_id;
                    break;
                }
            }
        }
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

static void kb_macro_key(struct nlk_keyboard_ctx *ctx, uint8_t kc, bool down)
{
    uint8_t i;
    if (kc >= NLK_KC_LCTRL && kc <= NLK_KC_RGUI) {
        uint8_t bit = (uint8_t)(1U << (kc - NLK_KC_LCTRL));
        if (down) ctx->macro_modifiers |= bit;
        else ctx->macro_modifiers &= (uint8_t)~bit;
        return;
    }
    if (kc == 0U) return;
    for (i = 0U; i < 6U; i++) {
        if (ctx->macro_keys[i] == kc) break;
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
    if (ctx->macro_modifiers != 0U) return true;
    for (i = 0U; i < 6U; i++) {
        if (ctx->macro_keys[i] != 0U) return true;
    }
    return false;
}

int nlk_cap_keyboard_macro_cancel(struct nlk_keyboard_ctx *ctx)
{
    bool held;
    if (ctx == NULL) return -EINVAL;
    held = kb_macro_has_held_keys(ctx);
    ctx->macro_playing = 0U;
    ctx->macro_tap_pending = 0U;
    ctx->macro_modifiers = 0U;
    memset(ctx->macro_keys, 0, sizeof(ctx->macro_keys));
    if (held) kb_rebuild_hid_report(ctx);
    return 0;
}

int nlk_cap_keyboard_macro_start(struct nlk_keyboard_ctx *ctx, uint8_t macro_id,
                                 uint32_t now_ms)
{
    uint8_t count;
    if (ctx == NULL || macro_id >= NLK_KB_MAX_MACROS) return -EINVAL;
    if (ctx->macro_playing != 0U || ctx->recording_macro_id < NLK_KB_MAX_MACROS) return -EBUSY;
    count = ctx->macro_info[macro_id].step_count;
    if (count == 0U || !kb_macro_steps_valid(ctx->macro_steps[macro_id], count)) return -EINVAL;
    memcpy(ctx->macro_play_steps, ctx->macro_steps[macro_id],
           (size_t)count * sizeof(ctx->macro_play_steps[0]));
    ctx->macro_play_count = count;
    ctx->macro_play_index = 0U;
    ctx->macro_play_id = macro_id;
    ctx->macro_repeats_left = ctx->macro_info[macro_id].repeat_count;
    if (ctx->macro_repeats_left == 0U) ctx->macro_repeats_left = 1U;
    ctx->macro_tap_pending = 0U;
    ctx->macro_playing = 1U;
    ctx->macro_clock_ms = now_ms;
    ctx->macro_due_ms = now_ms + ctx->macro_play_steps[0].delay_ms;
    return 0;
}

int nlk_cap_keyboard_tick(struct nlk_keyboard_ctx *ctx, uint32_t now_ms)
{
    const struct nlk_kb_macro_step *step;
    uint32_t delay;
    if (ctx == NULL) return -EINVAL;
    ctx->macro_clock_ms = now_ms;
    if (ctx->macro_playing == 0U || (int32_t)(now_ms - ctx->macro_due_ms) < 0) return 0;
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
    } else if (step->type == NLK_KB_MACRO_ACT_KEY_DOWN ||
               step->type == NLK_KB_MACRO_ACT_KEY_UP) {
        kb_macro_key(ctx, step->key_code, step->type == NLK_KB_MACRO_ACT_KEY_DOWN);
        kb_rebuild_hid_report(ctx);
    }
    ctx->macro_play_index++;
    if (ctx->macro_play_index >= ctx->macro_play_count) {
        ctx->macro_repeats_left--;
        if (ctx->macro_repeats_left == 0U) return nlk_cap_keyboard_macro_cancel(ctx);
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

static void kb_play_macro(struct nlk_keyboard_ctx *ctx, uint8_t macro_id)
{
    if (ctx == NULL) return;
    if (ctx->macro_playing != 0U) {
        (void)nlk_cap_keyboard_macro_cancel(ctx);
        return;
    }
    (void)nlk_cap_keyboard_macro_start(ctx, macro_id, ctx->macro_clock_ms);
}

static int kb_action_macro_play(const struct key_action_context *ctx, void *priv)
{
    struct nlk_keyboard_ctx *kb = (struct nlk_keyboard_ctx *)priv;
    if (ctx != NULL && ctx->event_type == 1U && kb != NULL) {
        uint8_t macro_id = 0U;
        if (ctx->initial_code >= NLK_KB_MACRO_KEYCODE_BASE &&
            ctx->initial_code < NLK_KB_MACRO_KEYCODE_BASE + NLK_KB_MAX_MACROS) {
            macro_id = (uint8_t)(ctx->initial_code - NLK_KB_MACRO_KEYCODE_BASE);
        }
        kb_play_macro(kb, macro_id);
    }
    return KEY_ACTION_DROP;
}

static uint8_t kb_execute_action_chain(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                       uint8_t key_index, uint8_t initial_code,
                                       uint8_t pressed)
{
    struct nlk_kb_key_action_chain *chain;
    struct key_action_context act_ctx;
    uint8_t current_code = initial_code;
    uint8_t final_code = initial_code;
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
    act_ctx.reserved[0] = 0U;
    act_ctx.reserved[1] = 0U;

    for (k = 0U; k < chain->count && k < NLK_KB_MAX_KEY_ACTIONS; k++) {
        uint8_t act_id = chain->action_ids[k];
        for (a = 0U; a < ctx->action_count && a < NLK_KB_MAX_ACTIONS; a++) {
            if (ctx->action_descs[a].action_id == act_id) {
                if (ctx->action_handlers[a] != NULL) {
                    act_ctx.key_code = current_code; /* 收集上一个动作输出的按键值作为输入 */
                    int res = ctx->action_handlers[a](&act_ctx, ctx->action_handler_privs[a]);

                    /* 在使用时判断动作回调是否返回了按键值 */
                    if (res < 0) {
                        /* 动作明确请求拦截按键 (如宏播放、Profile 切换等) */
                        key_dropped = true;
                        current_code = 0U;
                        final_code = 0U;
                    } else if (res > 0 && res <= 0xFF) {
                        /* 动作输出了有效按键值，以最后一个输出的为准 */
                        current_code = (uint8_t)res;
                        final_code = (uint8_t)res;
                        key_dropped = false;
                    } else {
                        /* res == 0 (KEY_ACTION_PASS): 该动作未返回按键值 (如仅用于点灯/旁路通知)
                         * 不覆盖已有按键值，保持原有键码继续流转 */
                    }
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
    memset(ctx->keymap, 0, sizeof(ctx->keymap));
    memset(ctx->action_chains, 0, sizeof(ctx->action_chains));
    ctx->key_state = NULL;
    ctx->active_code = NULL;
    ctx->key_capacity = 0U;
    ctx->layer_capacity = 0U;
}

int nlk_cap_keyboard_init(struct nlk_node *node, struct nlk_keyboard_ctx *ctx,
                          uint8_t rows, uint8_t cols, uint8_t key_count,
                          nlk_kb_report_cb_t report_cb, void *priv)
{
    return nlk_cap_keyboard_init_sized(node, ctx, rows, cols, key_count,
                                       NLK_KB_MAX_LAYERS, report_cb, priv);
}

int nlk_cap_keyboard_init_sized(struct nlk_node *node, struct nlk_keyboard_ctx *ctx,
                                uint8_t rows, uint8_t cols, uint8_t key_count,
                                uint8_t layer_count, nlk_kb_report_cb_t report_cb,
                                void *priv)
{
    uint8_t l;
    size_t cells;
    int ret;

    if (ctx == NULL || key_count == 0U || key_count > NLK_KB_MAX_KEYS ||
        layer_count == 0U || layer_count > NLK_KB_MAX_LAYERS) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    cells = (size_t)key_count * layer_count;
    ctx->keymap[0] = kmalloc(cells, GFP_KERNEL);
    ctx->action_chains[0] = kmalloc(cells * sizeof(*ctx->action_chains[0]), GFP_KERNEL);
    ctx->key_state = kmalloc((size_t)key_count * 2U, GFP_KERNEL);
    if (ctx->keymap[0] == NULL || ctx->action_chains[0] == NULL || ctx->key_state == NULL) {
        nlk_cap_keyboard_deinit(ctx);
        return -ENOMEM;
    }
    memset(ctx->keymap[0], 0, cells);
    memset(ctx->action_chains[0], 0, cells * sizeof(*ctx->action_chains[0]));
    memset(ctx->key_state, 0, (size_t)key_count * 2U);
    ctx->active_code = ctx->key_state + key_count;
    ctx->key_capacity = key_count;
    ctx->layer_capacity = layer_count;
    for (l = 1U; l < layer_count; l++) {
        ctx->keymap[l] = ctx->keymap[0] + (size_t)l * key_count;
        ctx->action_chains[l] = ctx->action_chains[0] + (size_t)l * key_count;
    }
    ctx->node = node;
    ctx->desc.form_factor = kb_infer_form_factor(key_count);
    ctx->desc.switch_type = NLK_KB_SW_MECH;
    ctx->desc.kb_flags = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO |
                         NLK_KB_FLAG_MEDIA_KEYS | NLK_KB_FLAG_MODULAR_HUB |
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

    ctx->recording_macro_id = 0xFFU;
    ctx->active_layer = 0U;
    ctx->default_layer = 0U;
    ctx->active_layout_id = ctx->desc.form_factor;

    /* Initialize default layers */
    for (l = 0U; l < NLK_KB_MAX_LAYERS; l++) {
        ctx->layers[l].layer_id = l;
        ctx->layers[l].flags = (l == 0U) ? 0U : 1U;
        if (l == 0U) {
            memcpy(ctx->layers[l].name, "Base", 5);
        } else if (l == 1U) {
            memcpy(ctx->layers[l].name, "FN", 3);
        } else if (l == 2U) {
            memcpy(ctx->layers[l].name, "PN", 3);
        } else if (l == 3U) {
            memcpy(ctx->layers[l].name, "MO3", 4);
        } else {
            ctx->layers[l].name[0] = 'L';
            ctx->layers[l].name[1] = (char)('0' + l);
            ctx->layers[l].name[2] = '\0';
        }
    }

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
    ctx->default_params = ctx->params;

    /* Register built-in standard actions */
    (void)nlk_cap_keyboard_register_action(ctx, "light_toggle",
                                           "Toggle backlight", NULL, NULL);
    (void)nlk_cap_keyboard_register_action(ctx, "caps_lock",
                                           "Toggle caps lock", NULL, NULL);
    (void)nlk_cap_keyboard_register_action(ctx, "macro_play",
                                           "Macro playback", kb_action_macro_play, ctx);

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

    ctx->cap.cap_ver = 0x10U;
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
    ret = nlk_node_register_cap(node, &ctx->cap);
    if (ret < 0) {
        nlk_cap_keyboard_deinit(ctx);
    }
    return ret;
}

int nlk_cap_keyboard_configure(struct nlk_keyboard_ctx *ctx,
                               const struct nlk_kb_desc *desc)
{
    if (ctx == NULL || desc == NULL ||
        desc->key_count == 0U || desc->key_count > ctx->key_capacity ||
        desc->layer_count == 0U || desc->layer_count > ctx->layer_capacity) {
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

int nlk_cap_keyboard_set_zone_light_cb(struct nlk_keyboard_ctx *ctx,
                                       nlk_kb_zone_light_cb_t zone_light_cb,
                                       void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->zone_light_cb = zone_light_cb;
    ctx->zone_light_priv = priv;
    return 0;
}

int nlk_cap_keyboard_set_layout_cb(struct nlk_keyboard_ctx *ctx,
                                   nlk_kb_layout_change_cb_t layout_cb,
                                   void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->layout_cb = layout_cb;
    ctx->layout_priv = priv;
    return 0;
}

int nlk_cap_keyboard_set_param_cb(struct nlk_keyboard_ctx *ctx,
                                  nlk_kb_param_change_cb_t param_cb,
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
    ctx->layouts[ctx->layout_count] = *layout;
    if (ctx->layout_count == 0U) {
        ctx->active_layout_id = layout->layout_id;
        ctx->params.active_layout_id = layout->layout_id;
    }
    ctx->layout_count++;
    return 0;
}

const struct nlk_kb_effect_entry *nlk_cap_keyboard_find_light_effect(
    const struct nlk_keyboard_ctx *ctx, uint8_t effect_id)
{
    const struct nlk_kb_effect_entry *entry;

    if (ctx == NULL) {
        return NULL;
    }
    for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
        if (entry->info.effect_id == effect_id) {
            return entry;
        }
    }
    return NULL;
}

int nlk_cap_keyboard_register_light_effect(struct nlk_keyboard_ctx *ctx,
                                           const char *name, const char *desc,
                                           nlk_kb_effect_render_fn_t handler, void *priv)
{
    struct nlk_kb_effect_entry *entry;

    if (ctx == NULL || name == NULL || name[0] == '\0' ||
        strlen(name) >= NLK_KB_LIGHT_EFFECT_NAME_MAX) {
        return -EINVAL;
    }
    for (entry = ctx->light_effects; entry != NULL; entry = entry->next) {
        if (strcmp(entry->info.name, name) == 0) {
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
        entry->info.effect_id = (uint8_t)ctx->light_effect_count;
        strcpy(entry->info.name, name);
        if (desc != NULL) {
            strncpy(entry->info.desc, desc, NLK_KB_LIGHT_EFFECT_DESC_MAX - 1U);
            entry->info.desc[NLK_KB_LIGHT_EFFECT_DESC_MAX - 1U] = '\0';
        }
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
    if (desc != NULL) {
        strncpy(entry->info.desc, desc, NLK_KB_LIGHT_EFFECT_DESC_MAX - 1U);
        entry->info.desc[NLK_KB_LIGHT_EFFECT_DESC_MAX - 1U] = '\0';
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

int nlk_cap_keyboard_set_layer_desc(struct nlk_keyboard_ctx *ctx, uint8_t layer,
                                    const char *name, uint8_t flags)
{
    if (ctx == NULL || layer >= NLK_KB_MAX_LAYERS) {
        return -EINVAL;
    }
    ctx->layers[layer].layer_id = layer;
    ctx->layers[layer].flags = flags;
    if (name != NULL) {
        strncpy(ctx->layers[layer].name, name, NLK_KB_LAYER_NAME_MAX - 1);
        ctx->layers[layer].name[NLK_KB_LAYER_NAME_MAX - 1] = '\0';
    }
    return 0;
}

int nlk_cap_keyboard_register_action(struct nlk_keyboard_ctx *ctx,
                                     const char *name,
                                     const char *desc,
                                     key_action_handler_fn_t handler,
                                     void *priv)
{
    uint8_t i;

    if (ctx == NULL || name == NULL || name[0] == '\0') {
        return -EINVAL;
    }
    for (i = 0U; i < ctx->action_count; i++) {
        if (strncmp(ctx->action_descs[i].name, name, NLK_KB_ACTION_NAME_MAX) == 0) {
            ctx->action_handlers[i] = handler;
            ctx->action_handler_privs[i] = priv;
            if (desc != NULL) {
                strncpy(ctx->action_descs[i].desc, desc, NLK_KB_ACTION_DESC_MAX - 1);
                ctx->action_descs[i].desc[NLK_KB_ACTION_DESC_MAX - 1] = '\0';
            }
            return 0;
        }
    }
    if (ctx->action_count >= NLK_KB_MAX_ACTIONS) {
        return -ENOSPC;
    }
    i = ctx->action_count;
    ctx->action_descs[i].action_id = (uint8_t)(i + 1U);
    ctx->action_descs[i].flags = 0U;
    ctx->action_descs[i].reserved = 0U;
    strncpy(ctx->action_descs[i].name, name, NLK_KB_ACTION_NAME_MAX - 1);
    ctx->action_descs[i].name[NLK_KB_ACTION_NAME_MAX - 1] = '\0';
    if (desc != NULL) {
        strncpy(ctx->action_descs[i].desc, desc, NLK_KB_ACTION_DESC_MAX - 1);
        ctx->action_descs[i].desc[NLK_KB_ACTION_DESC_MAX - 1] = '\0';
    } else {
        ctx->action_descs[i].desc[0] = '\0';
    }
    ctx->action_handlers[i] = handler;
    ctx->action_handler_privs[i] = priv;
    ctx->action_count++;
    return 0;
}

int nlk_cap_keyboard_bind_key_action(struct nlk_keyboard_ctx *ctx,
                                     uint8_t layer,
                                     uint8_t key_index,
                                     const char *action_name,
                                     bool append)
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

int nlk_cap_keyboard_unbind_key_action(struct nlk_keyboard_ctx *ctx,
                                       uint8_t layer,
                                       uint8_t key_index,
                                       const char *action_name)
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

int nlk_cap_keyboard_bind_all_keys(struct nlk_keyboard_ctx *ctx,
                                   uint8_t layer_mask,
                                   const char *action_name,
                                   bool append)
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

int nlk_cap_keyboard_set_macro(struct nlk_keyboard_ctx *ctx, uint8_t macro_id,
                               const char *name,
                               const struct nlk_kb_macro_step *steps,
                               uint8_t step_count, uint8_t repeat_count)
{
    if (ctx == NULL || macro_id >= NLK_KB_MAX_MACROS ||
        !kb_macro_steps_valid(steps, step_count)) {
        return -EINVAL;
    }
    if (ctx->macro_playing != 0U && ctx->macro_play_id == macro_id) {
        (void)nlk_cap_keyboard_macro_cancel(ctx);
    }
    ctx->macro_info[macro_id].macro_id = macro_id;
    ctx->macro_info[macro_id].step_count = step_count;
    ctx->macro_info[macro_id].repeat_count = (repeat_count == 0U) ? 1U : repeat_count;
    if (name != NULL) {
        strncpy(ctx->macro_info[macro_id].name, name, NLK_KB_MACRO_NAME_MAX - 1);
        ctx->macro_info[macro_id].name[NLK_KB_MACRO_NAME_MAX - 1] = '\0';
    }
    if (steps != NULL && step_count > 0U) {
        memcpy(ctx->macro_steps[macro_id], steps,
               (size_t)step_count * sizeof(struct nlk_kb_macro_step));
    }
    return 0;
}

int nlk_cap_keyboard_record_macro_step(struct nlk_keyboard_ctx *ctx,
                                       uint8_t type, uint8_t key_code,
                                       uint16_t delay_ms)
{
    uint8_t mid;
    uint8_t cur_cnt;

    if (ctx == NULL || ctx->recording_macro_id >= NLK_KB_MAX_MACROS) {
        return -EINVAL;
    }
    mid = ctx->recording_macro_id;
    cur_cnt = ctx->macro_info[mid].step_count;
    if (cur_cnt >= NLK_KB_MAX_MACRO_STEPS) {
        return -ENOSPC;
    }
    ctx->macro_steps[mid][cur_cnt].type = type;
    ctx->macro_steps[mid][cur_cnt].key_code = key_code;
    ctx->macro_steps[mid][cur_cnt].delay_ms = delay_ms;
    ctx->macro_info[mid].step_count = (uint8_t)(cur_cnt + 1U);
    return 0;
}

static void kb_record_event(struct nlk_keyboard_ctx *ctx, uint8_t type, uint8_t key_code)
{
    uint32_t elapsed;
    uint8_t id;
    if (ctx->recording_macro_id >= NLK_KB_MAX_MACROS || key_code == 0U ||
        key_code >= NLK_KC_FN) return;
    id = ctx->recording_macro_id;
    elapsed = ctx->macro_clock_ms - ctx->last_rec_tick;
    while (elapsed > UINT16_MAX) {
        if (nlk_cap_keyboard_record_macro_step(ctx, NLK_KB_MACRO_ACT_DELAY_MS,
                                               0U, UINT16_MAX) != 0) goto overflow;
        elapsed -= UINT16_MAX;
    }
    if (nlk_cap_keyboard_record_macro_step(ctx, type, key_code, (uint16_t)elapsed) != 0) {
        goto overflow;
    }
    ctx->last_rec_tick = ctx->macro_clock_ms;
    return;
overflow:
    ctx->macro_info[id].flags |= NLK_KB_MACRO_FLAG_RECORD_OVERFLOW;
    ctx->recording_macro_id = 0xFFU;
}

int nlk_cap_keyboard_set_params(struct nlk_keyboard_ctx *ctx,
                                const struct nlk_kb_params *params)
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
                                 const uint8_t *key_codes,
                                 uint8_t count)
{
    if (ctx == NULL || layer >= ctx->desc.layer_count || count > ctx->desc.key_count) {
        return -EINVAL;
    }
    if (key_codes != NULL) {
        memcpy(ctx->keymap[layer], key_codes, count);
    }
    return 0;
}

int nlk_cap_keyboard_update_local_keys(struct nlk_keyboard_ctx *ctx,
                                       const uint32_t *raw_bitmap,
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
        uint8_t base_kc = ctx->keymap[0][i];
        if (base_kc == NLK_KC_FN || base_kc == NLK_KC_PN || base_kc == NLK_KC_MO3) {
            ctx->key_state[i] = pressed;
        }
    }
    layer = kb_resolve_active_layer(ctx);

    /* Second pass: process press/release transitions and fire key actions */
    for (i = 0U; i < limit; i++) {
        uint8_t pressed = (uint8_t)((raw_bitmap[i / 32U] >> (i % 32U)) & 0x01U);
        uint8_t base_kc = ctx->keymap[0][i];
        bool is_layer_key = (base_kc == NLK_KC_FN ||
                             base_kc == NLK_KC_PN ||
                             base_kc == NLK_KC_MO3);

        if (pressed != 0U) {
            if (ctx->key_state[i] == 0U || is_layer_key) {
                uint8_t kc = is_layer_key ? base_kc : ctx->keymap[layer][i];
                bool first_down = (ctx->key_state[i] == 0U && !is_layer_key);

                ctx->key_state[i] = 1U;
                if (first_down) {
                    uint8_t final_kc = kb_execute_action_chain(ctx, layer, i, kc, 1U);
                    ctx->active_code[i] = final_kc;
                    if (ctx->recording_macro_id < NLK_KB_MAX_MACROS) {
                        kb_record_event(ctx, NLK_KB_MACRO_ACT_KEY_DOWN, final_kc);
                    }
                } else {
                    ctx->active_code[i] = kc;
                }
            }
        } else {
            if (ctx->key_state[i] != 0U) {
                (void)kb_execute_action_chain(ctx, layer, i, ctx->active_code[i], 0U);
                if (ctx->recording_macro_id < NLK_KB_MAX_MACROS) {
                    kb_record_event(ctx, NLK_KB_MACRO_ACT_KEY_UP, ctx->active_code[i]);
                }
            }
            ctx->key_state[i] = 0U;
            ctx->active_code[i] = 0U;
        }
    }

    kb_rebuild_hid_report(ctx);
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
            memcpy(named.effect_name, entry->info.name, sizeof(named.effect_name));
            len = sizeof(named);
        }
    }

    return nlk_node_send_event(ctx->node, NLK_ADDR_BROADCAST,
                               NLK_CAP_ID_BROADCAST,
                               NLK_KB_EVT_LIGHT_SYNC_BEACON,
                               &named, len, false);
}

int nlk_cap_keyboard_set_profile_cbs(struct nlk_keyboard_ctx *ctx,
                                     nlk_kb_profile_save_cb_t save_cb,
                                     nlk_kb_profile_load_cb_t load_cb,
                                     nlk_kb_profile_info_cb_t info_cb,
                                     void *priv)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    ctx->profile_save_cb = save_cb;
    ctx->profile_load_cb = load_cb;
    ctx->profile_info_cb = info_cb;
    ctx->profile_priv = priv;
    return 0;
}
