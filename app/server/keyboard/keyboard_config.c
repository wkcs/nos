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
    struct nlk_kb_macro *macros;
    uint8_t macro_count;
    int bound_macros;
    bool macro_bindings;
    size_t macros_pos;
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

static int config_optional_string_ok(const char *s, size_t size)
{
    return s != NULL && memchr(s, '\0', size) != NULL;
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
                         uint8_t id, int write, int apply, bool collection)
{
    struct nlk_kb_macro_info info = {0};
    struct nlk_kb_macro_step steps[NLK_KB_MAX_MACRO_STEPS];
    char desc[NLK_KB_MACRO_DESC_MAX] = {0};
    uint8_t i;

    if (write) {
        const struct nlk_kb_macro *macro = nlk_cap_keyboard_find_macro(ctx, id);
        if (macro == NULL) { c->error = -ENOENT; return; }
        info = macro->info;
        memcpy(desc, macro->desc, sizeof(desc));
        memcpy(steps, macro->steps, (size_t)info.step_count * sizeof(*steps));
    }
    config_io(c, &info, sizeof(info), write);
    config_io(c, desc, sizeof(desc), write);
    if (c->error != 0 || info.step_count > NLK_KB_MAX_MACRO_STEPS ||
        !config_optional_id_ok(info.id, sizeof(info.id)) ||
        !config_optional_string_ok(info.name, sizeof(info.name)) ||
        !config_optional_string_ok(desc, sizeof(desc))) {
        c->error = -EINVAL;
        return;
    }
    config_io(c, steps, (size_t)info.step_count * sizeof(*steps), write);
    for (i = 0U; i < info.step_count && c->error == 0; i++) {
        if (steps[i].type < NLK_KB_MACRO_ACT_KEY_DOWN ||
            steps[i].type > NLK_KB_MACRO_ACT_DELAY_MS ||
            (steps[i].type != NLK_KB_MACRO_ACT_DELAY_MS &&
             (steps[i].key_code == 0U || steps[i].key_code >= 0xE8U))) {
            c->error = -EINVAL;
        }
    }
    if (write || c->error != 0) return;
    if (!collection && apply) {
        const struct nlk_kb_macro *existing;
        if (id == NLK_KB_MACRO_NEW) {
            for (existing = ctx->macros; existing != NULL; existing = existing->next) {
                if (info.id[0] != '\0' && strcmp(info.id, existing->info.id) == 0) {
                    c->error = -EEXIST;
                    return;
                }
            }
            id = nlk_cap_keyboard_next_macro_id(ctx);
        }
        c->error = id == NLK_KB_MACRO_NEW ? -ENOSPC :
            nlk_cap_keyboard_set_macro_meta(ctx, id, info.id, info.name, desc,
                                           steps, info.step_count, info.repeat_count);
    } else if (collection && !apply) {
        struct nlk_kb_macro *macro = nlk_cap_keyboard_alloc_macro(info.step_count);
        struct nlk_kb_macro **link = &c->macros;
        if (macro == NULL) { c->error = -ENOMEM; return; }
        macro->info = info;
        macro->info.macro_id = id;
        memcpy(macro->desc, desc, sizeof(desc));
        memcpy(macro->steps, steps, (size_t)info.step_count * sizeof(*steps));
        while (*link != NULL && (*link)->info.macro_id < id) link = &(*link)->next;
        macro->next = *link;
        *link = macro;
        c->macro_count++;
    }
}

/* Only macro_play actions interpret the high 24 keycode bits as a macro hash. */
int keyboard_config_macro_bound(const struct nlk_keyboard_ctx *ctx, uint8_t slot,
                               uint32_t hash, uint32_t name_hash)
{
    uint8_t layer, key, action;

    if (ctx == NULL) return 0;
    for (layer = 0U; layer < ctx->desc.layer_count; layer++) {
        for (key = 0U; key < ctx->desc.key_count; key++) {
            const struct nlk_kb_key_action_chain *chain = &ctx->action_chains[layer][key];
            uint32_t code = ctx->keymap[layer][key];
            for (action = 0U; action < chain->count; action++) {
                uint8_t id = chain->action_ids[action];
                if (id == 0U || id > ctx->action_count ||
                    strcmp(ctx->action_descs[id - 1U].name, "macro_play") != 0) continue;
                if ((code >> 8) != 0U) {
                    if ((hash != 0U && (code >> 8) == hash) ||
                        (name_hash != 0U && (code >> 8) == name_hash)) return 1;
                } else if (slot != NLK_KB_MACRO_NEW) {
                    uint8_t bound_slot = code >= NLK_KB_MACRO_KEYCODE_BASE && code <= 0xFFU ?
                        (uint8_t)(code - NLK_KB_MACRO_KEYCODE_BASE) : 0U;
                    if (slot == bound_slot) return 1;
                }
            }
        }
    }
    return 0;
}

static int config_include_macro(const struct config_cursor *c,
                                 const struct nlk_keyboard_ctx *ctx,
                                 const struct nlk_kb_macro *macro)
{
    if (!c->bound_macros) return 1;
    return keyboard_config_macro_bound(ctx, macro->info.macro_id,
             macro->info.id[0] != '\0' ? nlk_fnv1a_24(macro->info.id) : 0U,
             macro->info.name[0] != '\0' ? nlk_fnv1a_24(macro->info.name) : 0U);
}

static void config_macros(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                          int write, int apply)
{
    const struct nlk_kb_macro *macro = ctx->macros;
    uint8_t count = 0U;
    uint8_t seen[32] = {0};
    unsigned int i;

    c->macros_pos = c->pos;
    if (write) {
        for (macro = ctx->macros; macro != NULL; macro = macro->next) {
            if (config_include_macro(c, ctx, macro)) count++;
        }
        macro = ctx->macros;
    }
    config_io(c, &count, 1U, write);
    for (i = 0U; i < count && c->error == 0; i++) {
        uint8_t id;
        while (write && macro != NULL && !config_include_macro(c, ctx, macro)) macro = macro->next;
        id = write ? macro->info.macro_id : 0U;
        config_io(c, &id, 1U, write);
        if (c->error != 0 || id == NLK_KB_MACRO_NEW ||
            (seen[id / 8U] & (1U << (id % 8U))) != 0U) {
            c->error = -EINVAL;
            return;
        }
        seen[id / 8U] |= (uint8_t)(1U << (id % 8U));
        config_macro(c, ctx, id, write, apply, true);
        if (write) macro = macro->next;
    }
    if (!write && apply && c->error == 0) {
        nlk_cap_keyboard_replace_macros(ctx, c->macros, c->macro_count);
        c->macros = NULL;
    }
}

static void config_keyboard(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                            int write, int apply)
{
    struct nlk_kb_params params;
    struct nlk_kb_analog_cfg analog;
    struct nlk_kb_light_override light_override = {0};
    uint8_t layer, key, i;
    int layout_valid = 0;

    if (write) {
        params = ctx->params;
        params.default_layer = ctx->default_layer;
        params.active_layout_id = ctx->active_layout_id;
        analog = ctx->analog_cfg;
        light_override = ctx->active_light_override;
    }
    config_io(c, &params, sizeof(params), write);
    config_io(c, &analog, sizeof(analog), write);
    config_io(c, &light_override, sizeof(light_override), write);
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
            uint32_t code = 0U;
            memset(&chain, 0, sizeof(chain));
            if (write) {
                code = ctx->keymap[layer][key];
                chain = ctx->action_chains[layer][key];
            }
            config_io(c, &code, sizeof(code), write);
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
                if (strcmp(name, "macro_play") == 0) c->macro_bindings = true;
            }
            if (!write && apply) {
                ctx->keymap[layer][key] = code;
                ctx->action_chains[layer][key] = chain;
            }
        }
    }
    if (!write && apply && c->error == 0) {
        (void)nlk_cap_keyboard_set_params(ctx, &params);
        ctx->default_layer = params.default_layer;
        ctx->active_layer = params.default_layer;
        ctx->active_layout_id = params.active_layout_id;
        ctx->analog_cfg = analog;
        ctx->active_light_override = light_override;
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

static void config_light_layers(struct config_cursor *c, struct nlk_keyboard_ctx *ctx,
                                 int write, int apply)
{
    uint8_t count = write ? ctx->light_layer_count : 0U;
    uint8_t i;

    config_io(c, &count, sizeof(count), write);
    if (c->error != 0 || count > CONFIG_KEYBOARD_LIGHT_MAX_LAYERS ||
        sizeof(struct nlk_kb_light_layers_hdr) + (size_t)count * sizeof(struct nlk_kb_light_layer) >
            CONFIG_NLK_PACKET_MTU - sizeof(int16_t)) {
        c->error = -EINVAL;
        return;
    }
    for (i = 0U; i < count; i++) {
        struct nlk_kb_light_layer layer = {0};

        if (write) {
            layer = ctx->light_layers[i];
        }
        config_io(c, &layer, sizeof(layer), write);
        if (layer.zone_id == NLK_KB_LIGHT_LAYER_OWN_CFG) {
            config_light_item(c, ctx, &layer.cfg.effect_id, write, 0);
        }
        if (c->error != 0 || nlk_cap_keyboard_validate_light_layer(ctx, &layer) != 0) {
            c->error = -EINVAL;
            return;
        }
        if (!write && apply) {
            ctx->light_layers[i] = layer;
        }
    }
    if (!write && apply) {
        c->error = nlk_cap_keyboard_set_light_layers(ctx, ctx->light_layers, count);
    }
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
    if (c->error == 0) {
        config_light_layers(c, ctx, write, apply);
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
    if (c->error == 0) {
        config_macros(c, ctx, write, apply);
    }
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
        if (c->error == 0) {
            config_macros(c, ctx, write, apply);
        }
    } else if (kind == KEYBOARD_CONFIG_LIGHT) {
        config_light(c, ctx, write, apply, 0);
    } else if (kind == KEYBOARD_CONFIG_MACRO) {
        config_macro(c, ctx, macro_id, write, apply, false);
    } else if (kind == KEYBOARD_CONFIG_ACTIVE) {
        config_active(c, ctx, write, apply);
    } else {
        c->error = -EINVAL;
    }
}

static int config_encode(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                          const char *name, const char *desc, uint8_t kind, uint8_t macro_id,
                          void *buffer, size_t capacity, size_t *length, int bound_macros)
{
    struct keyboard_config_header header;
    struct config_cursor c = {0};
    if (ctx == NULL || length == NULL || (buffer != NULL && capacity < sizeof(header)) ||
        !config_string_ok(hardware, sizeof(header.hardware)) ||
        !config_string_ok(name, sizeof(header.name))) {
        return -EINVAL;
    }
    memset(&header, 0, sizeof(header));
    header.magic = KEYBOARD_CONFIG_MAGIC;
    header.version = keyboard_config_version(kind);
    header.kind = kind;
    header.key_count = ctx->desc.key_count;
    header.layer_count = ctx->desc.layer_count;
    memcpy(header.hardware, hardware, strlen(hardware) + 1U);
    memcpy(header.name, name, strlen(name) + 1U);
    if (desc != NULL && config_optional_string_ok(desc, sizeof(header.desc))) {
        memcpy(header.desc, desc, strlen(desc) + 1U);
    }
    c.data = buffer;
    c.size = buffer != NULL ? capacity : (size_t)-1;
    c.pos = sizeof(header);
    c.error = 0;
    c.bound_macros = bound_macros;
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

int keyboard_config_encode(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                           const char *name, const char *desc, uint8_t kind, uint8_t macro_id,
                           void *buffer, size_t capacity, size_t *length)
{
    return config_encode(ctx, hardware, name, desc, kind, macro_id,
                          buffer, capacity, length, 0);
}

int keyboard_config_encode_persistent(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                                      const char *name, const char *desc, uint8_t kind, uint8_t macro_id,
                                      void *buffer, size_t capacity, size_t *length)
{
    return config_encode(ctx, hardware, name, desc, kind, macro_id,
                          buffer, capacity, length, 1);
}

static int config_decode(struct nlk_keyboard_ctx *ctx, const char *hardware,
                           uint8_t kind, uint8_t macro_id,
                           const void *buffer, size_t length, bool apply,
                           bool *macro_bindings, struct config_cursor *parsed)
{
    struct keyboard_config_header header, check;
    struct config_cursor c = {0};
    uint32_t crc;
    int pass;
    if (ctx == NULL || buffer == NULL || length < sizeof(header) ||
        !config_string_ok(hardware, sizeof(header.hardware))) {
        return -EINVAL;
    }
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != KEYBOARD_CONFIG_MAGIC || header.version != keyboard_config_version(kind) ||
        header.kind != kind || header.length != length ||
        header.key_count != ctx->desc.key_count || header.layer_count != ctx->desc.layer_count ||
        !config_string_ok(header.hardware, sizeof(header.hardware)) ||
        !config_string_ok(header.name, sizeof(header.name)) || strcmp(header.hardware, hardware) != 0) {
        return -EINVAL;
    }
    if (!config_optional_string_ok(header.desc, sizeof(header.desc))) {
        return -EINVAL;
    }
    check = header;
    check.checksum = 0U;
    crc = nlk_crc32(0U, &check, sizeof(check));
    crc = nlk_crc32(crc, (const uint8_t *)buffer + sizeof(header), length - sizeof(header));
    if (crc != header.checksum) {
        return -EIO;
    }
    for (pass = 0; pass < (apply ? 2 : 1); pass++) {
        c.data = (uint8_t *)buffer;
        c.size = length;
        c.pos = sizeof(header);
        c.error = 0;
        config_body(&c, ctx, kind, macro_id, 0, pass);
        if (c.error != 0 || c.pos != length) {
            nlk_cap_keyboard_free_macros(c.macros);
            return c.error != 0 ? c.error : -EINVAL;
        }
    }
    if (parsed != NULL) *parsed = c;
    else if (!apply) nlk_cap_keyboard_free_macros(c.macros);
    if (macro_bindings != NULL) *macro_bindings = c.macro_bindings;
    return 0;
}

int keyboard_config_decode(struct nlk_keyboard_ctx *ctx, const char *hardware,
                           uint8_t kind, uint8_t macro_id,
                           const void *buffer, size_t length)
{
    return config_decode(ctx, hardware, kind, macro_id, buffer, length, true, NULL, NULL);
}

int keyboard_config_validate(struct nlk_keyboard_ctx *ctx, const char *hardware,
                             uint8_t kind, uint8_t macro_id,
                             const void *buffer, size_t length, bool *macro_bindings)
{
    return config_decode(ctx, hardware, kind, macro_id, buffer, length, false, macro_bindings, NULL);
}

/* Slots belong to individual snapshots; stable IDs identify the library object. */
static int config_macro_target(const struct nlk_kb_macro_info *info,
                                const struct keyboard_config_macro_target *target)
{
    if (target->file_id[0] != '\0' && strcmp(info->id, target->file_id) == 0) return 1;
    if (target->info.id[0] != '\0') return strcmp(info->id, target->info.id) == 0;
    return target->file_id[0] == '\0' && info->id[0] == '\0' &&
        info->macro_id == target->info.macro_id;
}

static int config_macro_code_target(uint32_t code, const struct nlk_kb_macro *macros,
                                     const struct keyboard_config_macro_target *target)
{
    const struct nlk_kb_macro *macro;
    uint32_t hash = code >> 8;
    uint8_t slot = code >= NLK_KB_MACRO_KEYCODE_BASE && code <= 0xFFU ?
        (uint8_t)(code - NLK_KB_MACRO_KEYCODE_BASE) : 0U;

    if (hash != 0U) {
        if ((target->file_id[0] != '\0' && hash == nlk_fnv1a_24(target->file_id)) ||
            (target->info.id[0] != '\0' && hash == nlk_fnv1a_24(target->info.id)) ||
            (target->info.name[0] != '\0' && hash == nlk_fnv1a_24(target->info.name))) return 1;
        for (macro = macros; macro != NULL; macro = macro->next) {
            if (config_macro_target(&macro->info, target) && macro->info.name[0] != '\0' &&
                hash == nlk_fnv1a_24(macro->info.name)) return 1;
        }
        return 0;
    }
    for (macro = macros; macro != NULL; macro = macro->next) {
        if (macro->info.macro_id == slot) return config_macro_target(&macro->info, target);
    }
    return target->info.macro_id != NLK_KB_MACRO_NEW && slot == target->info.macro_id;
}

int keyboard_config_remove_macro(struct nlk_keyboard_ctx *ctx,
                                  const struct keyboard_config_macro_target *target)
{
    struct nlk_kb_macro **link;
    uint8_t layer, key;
    int changed = 0;

    if (ctx == NULL || target == NULL || !config_optional_id_ok(target->file_id, sizeof(target->file_id)) ||
        !config_optional_id_ok(target->info.id, sizeof(target->info.id)) ||
        (target->file_id[0] == '\0' && target->info.id[0] == '\0' && target->info.macro_id == NLK_KB_MACRO_NEW) ||
        !config_optional_string_ok(target->info.name, sizeof(target->info.name))) return -EINVAL;
    for (layer = 0U; layer < ctx->desc.layer_count; layer++) {
        for (key = 0U; key < ctx->desc.key_count; key++) {
            struct nlk_kb_key_action_chain *chain = &ctx->action_chains[layer][key];
            uint32_t code = ctx->keymap[layer][key];
            uint8_t in, out = 0U;
            if (!config_macro_code_target(code, ctx->macros, target)) continue;
            for (in = 0U; in < chain->count; in++) {
                const char *name = config_action_name(ctx, chain->action_ids[in]);
                if (name != NULL && strcmp(name, "macro_play") == 0) continue;
                chain->action_ids[out++] = chain->action_ids[in];
            }
            if (out == chain->count) continue;
            memset(chain->action_ids + out, 0, sizeof(chain->action_ids) - out);
            chain->count = out;
            ctx->keymap[layer][key] = (code >> 8) != 0U ? code & 0xFFU : 0U;
            changed = 1;
        }
    }
    link = &ctx->macros;
    while (*link != NULL) {
        struct nlk_kb_macro *macro = *link;
        if (!config_macro_target(&macro->info, target)) {
            link = &macro->next;
            continue;
        }
        if (ctx->macro_playing && ctx->macro_play_id == macro->info.macro_id) {
            (void)nlk_cap_keyboard_macro_cancel(ctx);
        }
        if (ctx->recording_macro_id == macro->info.macro_id) ctx->recording_macro_id = NLK_KB_MACRO_NEW;
        *link = macro->next;
        macro->next = NULL;
        nlk_cap_keyboard_free_macros(macro);
        ctx->macro_count--;
        changed = 1;
    }
    return changed;
}

/* Compact only the current serialized layout. No callbacks or runtime changes. */
int keyboard_config_strip_macro(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                 const struct keyboard_config_macro_target *target,
                                 void *buffer, size_t *length)
{
    struct keyboard_config_header header;
    struct config_cursor parsed = {0};
    uint8_t *data = buffer;
    size_t input, output, count_pos;
    unsigned int cell;
    uint8_t count, kept = 0U;
    int ret, changed = 0;

    if (buffer == NULL || length == NULL || *length < sizeof(header) || target == NULL ||
        !config_optional_id_ok(target->file_id, sizeof(target->file_id)) || target->file_id[0] == '\0' ||
        !config_optional_id_ok(target->info.id, sizeof(target->info.id)) ||
        !config_optional_string_ok(target->info.name, sizeof(target->info.name))) return -EINVAL;
    memcpy(&header, buffer, sizeof(header));
    if (header.kind != KEYBOARD_CONFIG_KEYBOARD && header.kind != KEYBOARD_CONFIG_ACTIVE) return -EINVAL;
    ret = config_decode(ctx, hardware, header.kind, 0U, buffer, *length, false, NULL, &parsed);
    if (ret != 0) return ret;
    input = sizeof(header) + (header.kind == KEYBOARD_CONFIG_ACTIVE ? KEYBOARD_CONFIG_ID_SIZE * 2U : 0U) +
        sizeof(struct nlk_kb_params) + sizeof(struct nlk_kb_analog_cfg) + sizeof(struct nlk_kb_light_override);
    output = input;
    for (cell = 0U; cell < (unsigned int)header.key_count * header.layer_count; cell++) {
        uint32_t code;
        size_t record = output;
        uint8_t action, actions, remaining = 0U;
        int matches;
        memcpy(&code, data + input, sizeof(code));
        input += sizeof(code);
        actions = data[input++];
        matches = config_macro_code_target(code, parsed.macros, target);
        output += sizeof(code) + 1U;
        for (action = 0U; action < actions; action++) {
            if (matches && strcmp((const char *)data + input, "macro_play") == 0) {
                changed = 1;
            } else {
                memmove(data + output, data + input, NLK_KB_ACTION_NAME_MAX);
                output += NLK_KB_ACTION_NAME_MAX;
                remaining++;
            }
            input += NLK_KB_ACTION_NAME_MAX;
        }
        if (remaining != actions) code = (code >> 8) != 0U ? code & 0xFFU : 0U;
        memcpy(data + record, &code, sizeof(code));
        data[record + sizeof(code)] = remaining;
    }
    /* Active lighting lies between the keyboard records and macro collection. */
    memmove(data + output, data + input, parsed.macros_pos - input);
    output += parsed.macros_pos - input;
    input = parsed.macros_pos;
    count = data[input++];
    count_pos = output++;
    while (count-- != 0U) {
        struct nlk_kb_macro_info info;
        size_t size;
        memcpy(&info, data + input + 1U, sizeof(info));
        size = 1U + sizeof(info) + NLK_KB_MACRO_DESC_MAX +
            (size_t)info.step_count * sizeof(struct nlk_kb_macro_step);
        if (config_macro_target(&info, target)) {
            changed = 1;
        } else {
            memmove(data + output, data + input, size);
            output += size;
            kept++;
        }
        input += size;
    }
    data[count_pos] = kept;
    nlk_cap_keyboard_free_macros(parsed.macros);
    if (!changed) return 0;
    header.length = (uint32_t)output;
    header.checksum = 0U;
    memcpy(data, &header, sizeof(header));
    header.checksum = nlk_crc32(0U, data, output);
    memcpy(data, &header, sizeof(header));
    *length = output;
    return 1;
}
