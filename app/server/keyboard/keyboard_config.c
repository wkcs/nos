/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Compact, hardware-specific keyboard configuration objects.
 */
#include <nos/keyboard_config.h>
#include <nos/nlk_bearer.h>
#include <uapi/nos/errno.h>
#include <string.h>

struct config_cursor {
    uint8_t *data;
    size_t size;
    size_t pos;
    int error;
};

static void config_io(struct config_cursor *c, void *item, size_t size, int write)
{
    if (c->error != 0 || size > c->size - c->pos) {
        c->error = -EINVAL;
        return;
    }
    if (write && c->data != NULL) {
        memcpy(c->data + c->pos, item, size);
    } else if (!write) {
        memcpy(item, c->data + c->pos, size);
    }
    c->pos += size;
}

static int config_string_ok(const char *s, size_t size)
{
    return s != NULL && s[0] != '\0' && memchr(s, '\0', size) != NULL;
}

static int config_optional_id_ok(const char *s, size_t size)
{
    size_t i;
    if (s == NULL) {
        return 0;
    }
    for (i = 0U; i < size; i++) {
        char c = s[i];
        if (c == '\0') {
            return 1;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 0;
}

static uint8_t config_action_id(const struct nlk_keyboard_ctx *ctx, const char *name)
{
    uint8_t i;
    if (!config_string_ok(name, NLK_KB_ACTION_NAME_MAX)) {
        return 0U;
    }
    for (i = 0U; i < ctx->action_count; i++) {
        if (strncmp(ctx->action_descs[i].name, name, NLK_KB_ACTION_NAME_MAX) == 0) {
            return ctx->action_descs[i].action_id;
        }
    }
    return 0U;
}

static const char *config_action_name(const struct nlk_keyboard_ctx *ctx, uint8_t id)
{
    uint8_t i;
    for (i = 0U; i < ctx->action_count; i++) {
        if (ctx->action_descs[i].action_id == id) {
            return ctx->action_descs[i].name;
        }
    }
    return NULL;
}

static int config_effect_id(const struct nlk_keyboard_ctx *ctx, const char *name)
{
    const struct nlk_kb_effect_entry *entry = ctx->light_effects;
    if (!config_string_ok(name, NLK_KB_LIGHT_EFFECT_NAME_MAX)) {
        return -EINVAL;
    }
    while (entry != NULL) {
        if (strncmp(entry->info.name, name, NLK_KB_LIGHT_EFFECT_NAME_MAX) == 0) {
            return entry->info.effect_id;
        }
        entry = entry->next;
    }
    return -EINVAL;
}

static void config_macro(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                         uint8_t slot, int write, int apply)
{
    struct nlk_kb_macro_info info;
    struct nlk_kb_macro_step step;
    uint8_t i;

    if (slot >= NLK_KB_MAX_MACROS) {
        c->error = -EINVAL;
        return;
    }
    memset(&info, 0, sizeof(info));
    if (write) {
        info = ctx->macro_info[slot];
        info.macro_id = slot;
    }
    config_io(c, &info, sizeof(info), write);
    if (c->error != 0 || info.step_count > NLK_KB_MAX_MACRO_STEPS ||
        (info.step_count != 0U && !config_string_ok(info.name, sizeof(info.name)))) {
        c->error = -EINVAL;
        return;
    }
    if (!write && apply) {
        if (ctx->macro_playing != 0U && ctx->macro_play_id == slot) {
            (void)nlk_cap_keyboard_macro_cancel(ctx);
        }
        info.macro_id = slot;
        ctx->macro_info[slot] = info;
        memset(ctx->macro_steps[slot], 0, sizeof(ctx->macro_steps[slot]));
    }
    for (i = 0U; i < info.step_count; i++) {
        if (write) {
            step = ctx->macro_steps[slot][i];
        }
        config_io(c, &step, sizeof(step), write);
        if (c->error != 0 || step.type < NLK_KB_MACRO_ACT_KEY_DOWN ||
            step.type > NLK_KB_MACRO_ACT_DELAY_MS ||
            (step.type != NLK_KB_MACRO_ACT_DELAY_MS &&
             (step.key_code == 0U || step.key_code >= 0xE8U))) {
            c->error = -EINVAL;
            return;
        }
        if (!write && apply) {
            ctx->macro_steps[slot][i] = step;
        }
    }
}

static void config_keyboard(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                            int write, int apply)
{
    struct nlk_kb_params params;
    struct nlk_kb_analog_cfg analog;
    uint8_t layer, key, i, macro_count = 0U;
    uint8_t seen = 0U;
    int layout_valid = 0;

    if (write) {
        params = ctx->params;
        params.default_layer = ctx->default_layer;
        params.active_layout_id = ctx->active_layout_id;
        analog = ctx->analog_cfg;
    }
    config_io(c, &params, sizeof(params), write);
    config_io(c, &analog, sizeof(analog), write);
    if (c->error != 0 || params.default_layer >= ctx->desc.layer_count) {
        c->error = -EINVAL;
        return;
    }
    if (ctx->layout_count == 0U) {
        layout_valid = params.active_layout_id == ctx->active_layout_id;
    } else {
        for (i = 0U; i < ctx->layout_count; i++) {
            if (ctx->layouts[i].layout_id == params.active_layout_id &&
                ctx->layouts[i].key_count == ctx->desc.key_count) {
                layout_valid = 1;
                break;
            }
        }
    }
    if (!layout_valid) {
        c->error = -EINVAL;
        return;
    }
    for (layer = 0U; layer < ctx->desc.layer_count; layer++) {
        for (key = 0U; key < ctx->desc.key_count; key++) {
            struct nlk_kb_key_action_chain chain;
            uint8_t code = 0U;
            memset(&chain, 0, sizeof(chain));
            if (write) {
                code = ctx->keymap[layer][key];
                chain = ctx->action_chains[layer][key];
            }
            config_io(c, &code, 1U, write);
            config_io(c, &chain.count, 1U, write);
            if (c->error != 0 || chain.count > NLK_KB_MAX_KEY_ACTIONS) {
                c->error = -EINVAL;
                return;
            }
            for (i = 0U; i < chain.count; i++) {
                char name[NLK_KB_ACTION_NAME_MAX] = {0};
                uint8_t id;
                if (write) {
                    const char *source = config_action_name(ctx, chain.action_ids[i]);
                    if (!config_string_ok(source, sizeof(name))) {
                        c->error = -EINVAL;
                        return;
                    }
                    memcpy(name, source, strlen(source) + 1U);
                }
                config_io(c, name, sizeof(name), write);
                id = config_action_id(ctx, name);
                if (c->error != 0 || id == 0U) {
                    c->error = -EINVAL;
                    return;
                }
                chain.action_ids[i] = id;
            }
            if (!write && apply) {
                ctx->keymap[layer][key] = code;
                ctx->action_chains[layer][key] = chain;
            }
        }
    }
    if (write) {
        for (i = 0U; i < NLK_KB_MAX_MACROS; i++) {
            if (ctx->macro_info[i].step_count != 0U) {
                macro_count++;
            }
        }
    }
    config_io(c, &macro_count, 1U, write);
    if (c->error != 0 || macro_count > NLK_KB_MAX_MACROS) {
        c->error = -EINVAL;
        return;
    }
    if (!write && apply) {
        if (ctx->macro_playing != 0U) {
            (void)nlk_cap_keyboard_macro_cancel(ctx);
        }
        memset(ctx->macro_info, 0, sizeof(ctx->macro_info));
        memset(ctx->macro_steps, 0, sizeof(ctx->macro_steps));
    }
    for (i = 0U; i < macro_count; i++) {
        uint8_t slot = 0U;
        if (write) {
            while (slot < NLK_KB_MAX_MACROS &&
                   (ctx->macro_info[slot].step_count == 0U || (seen & (1U << slot)) != 0U)) {
                slot++;
            }
        }
        config_io(c, &slot, 1U, write);
        if (c->error != 0 || slot >= NLK_KB_MAX_MACROS || (seen & (1U << slot)) != 0U) {
            c->error = -EINVAL;
            return;
        }
        seen |= (uint8_t)(1U << slot);
        config_macro(c, ctx, slot, write, apply);
    }
    if (!write && apply && c->error == 0) {
        (void)nlk_cap_keyboard_set_params(ctx, &params);
        ctx->default_layer = params.default_layer;
        ctx->active_layer = params.default_layer;
        ctx->active_layout_id = params.active_layout_id;
        ctx->analog_cfg = analog;
    }
}

static void config_light_item(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                              uint8_t *effect, int write, int allow_empty)
{
    char name[NLK_KB_LIGHT_EFFECT_NAME_MAX] = {0};
    int id;
    if (allow_empty && ctx->light_effect_count == 0U) {
        config_io(c, name, sizeof(name), write);
        if (c->error != 0 || name[0] != '\0') {
            c->error = -EINVAL;
        }
        return;
    }
    if (write) {
        const struct nlk_kb_effect_entry *entry = nlk_cap_keyboard_find_light_effect(ctx, *effect);
        if (entry == NULL || !config_string_ok(entry->info.name, sizeof(name))) {
            c->error = -EINVAL;
            return;
        }
        memcpy(name, entry->info.name, strlen(entry->info.name) + 1U);
    }
    config_io(c, name, sizeof(name), write);
    id = config_effect_id(ctx, name);
    if (c->error != 0 || id < 0) {
        c->error = -EINVAL;
        return;
    }
    *effect = (uint8_t)id;
}

static void config_light(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                         int write, int apply, int allow_empty)
{
    struct nlk_kb_light_cfg light;
    struct nlk_kb_zone_light_cfg zone;
    uint8_t count = 0U, i;

    if (write) {
        light = ctx->light_cfg;
        count = ctx->light_zone_count;
    }
    config_io(c, &light, sizeof(light), write);
    config_light_item(c, ctx, &light.effect_id, write, allow_empty);
    config_io(c, &count, 1U, write);
    if (c->error != 0 || count != ctx->light_zone_count || count > NLK_KB_MAX_LIGHT_ZONES) {
        c->error = -EINVAL;
        return;
    }
    if (!write && apply) {
        ctx->light_cfg = light;
    }
    for (i = 0U; i < count; i++) {
        if (write) {
            zone = ctx->zone_cfgs[i];
        }
        config_io(c, &zone, sizeof(zone), write);
        config_light_item(c, ctx, &zone.effect_id, write, allow_empty);
        if (c->error != 0 || zone.zone_id != i) {
            c->error = -EINVAL;
            return;
        }
        if (!write && apply) {
            ctx->zone_cfgs[i] = zone;
        }
    }
    if (!write && apply && c->error == 0) {
        if (ctx->light_cb != NULL) {
            ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
        }
        if (ctx->zone_light_cb != NULL) {
            for (i = 0U; i < count; i++) {
                ctx->zone_light_cb(i, &ctx->zone_cfgs[i], ctx->zone_light_priv);
            }
        }
    }
}

static void config_active(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                          int write, int apply)
{
    char kb_id[KEYBOARD_CONFIG_ID_SIZE] = {0};
    char light_id[KEYBOARD_CONFIG_ID_SIZE] = {0};

    if (write) {
        if (!config_optional_id_ok(ctx->active_keyboard_id, sizeof(kb_id)) ||
            !config_optional_id_ok(ctx->active_light_id, sizeof(light_id))) {
            c->error = -EINVAL;
            return;
        }
        memcpy(kb_id, ctx->active_keyboard_id, strlen(ctx->active_keyboard_id) + 1U);
        memcpy(light_id, ctx->active_light_id, strlen(ctx->active_light_id) + 1U);
    }
    config_io(c, kb_id, sizeof(kb_id), write);
    config_io(c, light_id, sizeof(light_id), write);
    if (c->error != 0 ||
        !config_optional_id_ok(kb_id, sizeof(kb_id)) ||
        !config_optional_id_ok(light_id, sizeof(light_id))) {
        c->error = -EINVAL;
        return;
    }
    config_keyboard(c, ctx, write, apply);
    if (c->error != 0) {
        return;
    }
    config_light(c, ctx, write, apply, 1);
    if (!write && apply && c->error == 0) {
        memcpy(ctx->active_keyboard_id, kb_id, sizeof(kb_id));
        memcpy(ctx->active_light_id, light_id, sizeof(light_id));
    }
}

static void config_body(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                        uint8_t kind, uint8_t macro_id, int write, int apply)
{
    if (kind == KEYBOARD_CONFIG_KEYBOARD) {
        config_keyboard(c, ctx, write, apply);
    } else if (kind == KEYBOARD_CONFIG_LIGHT) {
        config_light(c, ctx, write, apply, 0);
    } else if (kind == KEYBOARD_CONFIG_MACRO) {
        config_macro(c, ctx, macro_id, write, apply);
    } else if (kind == KEYBOARD_CONFIG_ACTIVE) {
        config_active(c, ctx, write, apply);
    } else {
        c->error = -EINVAL;
    }
}

int keyboard_config_encode(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                           const char *name, uint8_t kind, uint8_t macro_id,
                           void *buffer, size_t capacity, size_t *length)
{
    struct keyboard_config_header header;
    struct config_cursor c;
    if (ctx == NULL || length == NULL || (buffer != NULL && capacity < sizeof(header)) ||
        !config_string_ok(hardware, sizeof(header.hardware)) ||
        !config_string_ok(name, sizeof(header.name))) {
        return -EINVAL;
    }
    memset(&header, 0, sizeof(header));
    header.magic = KEYBOARD_CONFIG_MAGIC;
    header.version = 1U;
    header.kind = kind;
    header.key_count = ctx->desc.key_count;
    header.layer_count = ctx->desc.layer_count;
    memcpy(header.hardware, hardware, strlen(hardware) + 1U);
    memcpy(header.name, name, strlen(name) + 1U);
    c.data = buffer;
    c.size = buffer != NULL ? capacity : (size_t)-1;
    c.pos = sizeof(header);
    c.error = 0;
    config_body(&c, (struct nlk_keyboard_ctx *)ctx, kind, macro_id, 1, 0);
    if (c.error != 0) {
        return c.error;
    }
    if (buffer == NULL) {
        *length = c.pos;
        return 0;
    }
    header.length = (uint32_t)c.pos;
    memcpy(buffer, &header, sizeof(header));
    header.checksum = nlk_crc32(0U, buffer, c.pos);
    memcpy(buffer, &header, sizeof(header));
    *length = c.pos;
    return 0;
}

int keyboard_config_decode(struct nlk_keyboard_ctx *ctx, const char *hardware,
                           uint8_t kind, uint8_t macro_id,
                           const void *buffer, size_t length)
{
    struct keyboard_config_header header, check;
    struct config_cursor c;
    uint32_t crc;
    int pass;
    if (ctx == NULL || buffer == NULL || length < sizeof(header) ||
        !config_string_ok(hardware, sizeof(header.hardware))) {
        return -EINVAL;
    }
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != KEYBOARD_CONFIG_MAGIC || header.version != 1U ||
        header.kind != kind || header.length != length ||
        header.key_count != ctx->desc.key_count || header.layer_count != ctx->desc.layer_count ||
        !config_string_ok(header.hardware, sizeof(header.hardware)) ||
        !config_string_ok(header.name, sizeof(header.name)) || strcmp(header.hardware, hardware) != 0) {
        return -EINVAL;
    }
    check = header;
    check.checksum = 0U;
    crc = nlk_crc32(0U, &check, sizeof(check));
    crc = nlk_crc32(crc, (const uint8_t *)buffer + sizeof(header), length - sizeof(header));
    if (crc != header.checksum) {
        return -EIO;
    }
    for (pass = 0; pass < 2; pass++) {
        c.data = (uint8_t *)buffer;
        c.size = length;
        c.pos = sizeof(header);
        c.error = 0;
        config_body(&c, ctx, kind, macro_id, 0, pass);
        if (c.error != 0 || c.pos != length) {
            return -EINVAL;
        }
    }
    return 0;
}
