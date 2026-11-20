/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Keyboard configuration documents and compiled definitions.
 */
#include "keyboard_config_internal.h"
#include <kernel/mm.h>
#include <uapi/nos/errno.h>
#include <string.h>

static const struct kb_action_definition *find_action(const char *id)
{
    size_t i;
    for (i = 0; i < kb_action_definition_count; i++) {
        if (!strcmp(kb_action_definitions[i]->id, id)) {
            return kb_action_definitions[i];
        }
    }
    return NULL;
}
static const struct kb_effect_definition *find_effect(const char *id)
{
    size_t i;
    for (i = 0; i < kb_effect_definition_count; i++) {
        if (!strcmp(kb_effect_definitions[i]->id, id)) {
            return kb_effect_definitions[i];
        }
    }
    return NULL;
}
/* 参数结构校验同时用于配置提交与删除引用检查，正返回值表示命中目标引用 */
static int action_arguments(const struct kb_action_definition *definition, const void *bytes,
                            uint16_t length, const struct kb_store *store, uint8_t kind,
                            const char *name)
{
    struct kb_doc_reader reader;
    uint8_t i;
    bool found = false;

    kb_doc_reader_init(&reader, bytes, length);
    for (i = 0; i < definition->parameter_count; i++) {
        const struct kb_parameter_definition *field = &definition->parameters[i];

        if (field->type == KB_PARAMETER_U8) {
            uint32_t value = READ_UINT(&reader, 1);
            if (value < field->minimum || value > field->maximum) {
                return -EINVAL;
            }
        } else if (field->type == KB_PARAMETER_RGB) {
            (void)kb_doc_bytes(&reader, 3);
        } else {
            char target[KB_STORE_NAME_SIZE];
            uint8_t target_kind =
                field->type == KB_PARAMETER_BASE_REF ? KB_STORE_BASE : KB_STORE_MACRO;

            kb_doc_string(&reader, target, sizeof(target));
            if (reader.error || !kb_store_valid_name(target)) {
                return -EINVAL;
            }
            if (store && !kb_store_find(store, target_kind, target)) {
                return -ENOENT;
            }
            if (name && kind == target_kind && !strcmp(target, name)) {
                found = true;
            }
        }
    }
    if (reader.error || reader.left) {
        return -EINVAL;
    }
    if (definition->validate_arguments) {
        int rc = definition->validate_arguments(bytes, length);
        if (rc) {
            return rc;
        }
    }
    return found ? 1 : 0;
}
void kb_config_metadata(struct kb_doc_writer *w, const char *name, const char *description)
{
    size_t rec = kb_doc_record(w, NLK_KB_TAG_METADATA);
    kb_doc_str(w, name);
    kb_doc_str(w, description);
    kb_doc_end(w, rec);
}
static void style_write(struct kb_doc_writer *w, const struct nlk_kb_light_cfg *cfg)
{
    const struct kb_effect_definition *effect = NULL;
    size_t i;
    for (i = 0; i < kb_effect_definition_count; i++) {
        if (kb_effect_definitions[i]->engine_id == cfg->effect_id) {
            effect = kb_effect_definitions[i];
        }
    }
    if (effect == NULL) {
        w->error = -EINVAL;
        return;
    }
    kb_doc_str(w, effect->id);
    for (i = 0; i < 3; i++) {
        if (!(effect->fields & (1U << i))) {
            continue;
        }
        kb_doc_put(w, i + 1, 2);
        kb_doc_put(w, i == 2 ? 2 : 1, 1);
        kb_doc_put(w, i == 2 ? 3 : 1, 1);
        if (i == 0) {
            kb_doc_put(w, cfg->brightness, 1);
        } else if (i == 1) {
            kb_doc_put(w, cfg->speed, 1);
        } else {
            kb_doc_put(w, cfg->color_r, 1);
            kb_doc_put(w, cfg->color_g, 1);
            kb_doc_put(w, cfg->color_b, 1);
        }
    }
}
static int style_read(struct kb_doc_reader *r, struct nlk_kb_light_cfg *cfg,
                      struct kb_effect_parameters *parameters)
{
    char id[32];
    uint16_t seen = 0;
    const uint8_t *fields;
    size_t fields_length;
    const struct kb_effect_definition *effect;
    kb_doc_string(r, id, sizeof(id));
    effect = find_effect(id);
    if (r->error || effect == NULL) {
        return -EINVAL;
    }
    fields = r->data;
    fields_length = r->left;
    if (fields_length > KB_EFFECT_PARAMETERS_MAX) {
        return -ENOSPC;
    }
    parameters->length = fields_length;
    memcpy(parameters->data, fields, fields_length);
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable = 1;
    cfg->zone_mask = NLK_KB_LIGHT_ZONE_ALL;
    cfg->effect_id = effect->engine_id;
    cfg->brightness = 200;
    cfg->speed = 128;
    while (r->left && !r->error) {
        unsigned int field = READ_UINT(r, 2), type = READ_UINT(r, 1), len = READ_UINT(r, 1);
        const struct kb_parameter_definition *custom = NULL;
        size_t i;
        unsigned int bit = field > 0 && field <= 3 ? field - 1 : 16;
        for (i = 0; i < effect->parameter_count; i++) {
            if (effect->parameters[i].id == field) {
                custom = &effect->parameters[i];
                bit = i + 3;
                break;
            }
        }
        if (bit >= 16 || (seen & (1U << bit))) {
            return -EINVAL;
        }
        seen |= 1U << bit;
        if (custom != NULL) {
            const uint8_t *bytes = kb_doc_bytes(r, len);
            if (!bytes || type != custom->type || (type != 1 && type != 2) ||
                len != (type == 2 ? 3U : 1U) ||
                (type == 1 && (bytes[0] < custom->minimum || bytes[0] > custom->maximum))) {
                return -EINVAL;
            }
        } else {
            if (!(effect->fields & (1U << bit)) || type != (field == 3 ? 2U : 1U) ||
                len != (field == 3 ? 3U : 1U)) {
                return -EINVAL;
            }
            if (field == 1) {
                cfg->brightness = READ_UINT(r, 1);
            } else if (field == 2) {
                cfg->speed = READ_UINT(r, 1);
            } else {
                cfg->color_r = READ_UINT(r, 1);
                cfg->color_g = READ_UINT(r, 1);
                cfg->color_b = READ_UINT(r, 1);
            }
        }
    }
    return r->error;
}
int kb_config_default_document(struct keyboard_config_service *s, uint8_t kind, void *buffer,
                            size_t capacity)
{
    struct kb_doc_writer w;
    struct nlk_keyboard_ctx *kb = s->keyboard;
    size_t rec;
    uint8_t l, k, a;
    kb_doc_writer_init(&w, buffer, capacity);
    kb_config_metadata(&w, "default", "");
    if (kind == KB_STORE_BASE) {
        rec = kb_doc_record(&w, NLK_KB_TAG_BASE_HEADER);
        kb_doc_str(&w, kb_selected_layout.id);
        kb_doc_put(&w, kb_selected_layout.version, 2);
        kb_doc_put(&w, kb->default_layer, 1);
        kb_doc_put(&w, kb->params.debounce_ms, 1);
        kb_doc_put(&w, kb->params.poll_rate_hz, 2);
        kb_doc_put(&w, kb->params.nkro_enable, 1);
        kb_doc_put(&w, kb->params.win_lock, 1);
        kb_doc_put(&w, kb->params.fn_lock, 1);
        kb_doc_put(&w, kb->params.sleep_timeout_min, 1);
        kb_doc_end(&w, rec);
        for (l = 0; l < kb->desc.layer_count; l++) {
            for (k = 0; k < kb->desc.key_count; k++) {
                const struct nlk_kb_key_action_chain *chain = &kb->action_chains[l][k];
                rec = kb_doc_record(&w, NLK_KB_TAG_BASE_KEY);
                kb_doc_put(&w, l, 1);
                kb_doc_put(&w, k, 2);
                kb_doc_put(&w, kb->keymap[l][k] & 255U, 4);
                kb_doc_put(&w, chain->count, 1);
                for (a = 0; a < chain->count; a++) {
                    uint8_t i;
                    for (i = 0; i < kb->action_count; i++) {
                        if (nlk_cap_keyboard_action_id(kb, i) == chain->action_ids[a]) {
                            break;
                        }
                    }
                    if (i == kb->action_count) {
                        w.error = -EINVAL;
                        break;
                    }
                    kb_doc_str(&w, kb->action_names[i]);
                    kb_doc_put(&w, 0, 2);
                }
                kb_doc_end(&w, rec);
            }
        }
    } else if (kind == KB_STORE_LIGHT) {
        for (l = 0; l < kb->light_layer_count; l++) {
            struct nlk_kb_light_layer layer = kb->light_layers[l];
            const char *trigger = "";
            for (a = 0; a < kb->action_count; a++) {
                if (nlk_fnv1a_24(kb->action_names[a]) == layer.trigger) {
                    trigger = kb->action_names[a];
                }
            }
            rec = kb_doc_record(&w, NLK_KB_TAG_LIGHT_LAYER);
            kb_doc_put(&w, layer.x, 1);
            kb_doc_put(&w, layer.y, 1);
            kb_doc_put(&w, layer.width, 1);
            kb_doc_put(&w, layer.height, 1);
            kb_doc_put(&w, (uint16_t)layer.z_order, 2);
            kb_doc_put(&w, layer.opacity, 1);
            kb_doc_put(&w, layer.activation, 1);
            kb_doc_str(&w, trigger);
            style_write(&w, &layer.cfg);
            kb_doc_end(&w, rec);
        }
    }
    return w.error ? w.error : (int)w.used;
}
static int metadata_read(struct kb_doc_reader *reader, uint8_t *seen)
{
    char name[KB_STORE_NAME_SIZE];

    if (++*seen != 1 || kb_doc_string(reader, name, sizeof(name)) ||
        kb_doc_string(reader, NULL, 128) || !kb_store_valid_name(name)) {
        return -EINVAL;
    }
    return 0;
}

int kb_config_sync_metadata_name(struct keyboard_config_service *s, const char *name)
{
    struct kb_doc_reader reader, value;
    uint16_t tag;
    size_t offset = 0, meta_offset = 0, old_prefix_len = 0, desc_wire_len = 0, next_length;
    uint8_t meta_count = 0;
    char current_name[KB_STORE_NAME_SIZE];
    /* 原地仅移动后缀并替换 tag/len + 新名称，后面的 description 字节直接保留，避免在栈上分配 128B+ 缓冲 */
    uint8_t header_and_name[4U + 1U + KB_STORE_NAME_MAX];
    struct kb_doc_writer writer;
    uint8_t *buffer;
    int rc;

    if (s == NULL || s->draft == NULL || !kb_store_valid_name(name)) {
        return -EINVAL;
    }
    kb_doc_reader_init(&reader, s->draft, s->draft_length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        if (tag == NLK_KB_TAG_METADATA) {
            if (meta_count++ == 0U) {
                meta_offset = offset;
                if (kb_doc_string(&value, current_name, sizeof(current_name))) {
                    return -EINVAL;
                }
                desc_wire_len = value.left;
                old_prefix_len = 4U + 1U + strlen(current_name);
                if (kb_doc_string(&value, NULL, 128) || value.left) {
                    return -EINVAL;
                }
            }
        }
        offset = s->draft_length - reader.left;
    }
    if (rc < 0 || meta_count != 1U) {
        return -EINVAL;
    }
    if (!strcmp(current_name, name)) {
        return 0;
    }
    kb_doc_writer_init(&writer, header_and_name, sizeof(header_and_name));
    kb_doc_put(&writer, NLK_KB_TAG_METADATA, 2);
    kb_doc_put(&writer, 1U + strlen(name) + desc_wire_len, 2);
    kb_doc_str(&writer, name);
    if (writer.error) {
        return writer.error;
    }
    next_length = s->draft_length - old_prefix_len + writer.used;
    if (next_length > KB_CONFIG_BODY_MAX) {
        return -ENOSPC;
    }
    buffer = s->draft;
    if (next_length > s->draft_capacity) {
        buffer = kmalloc(next_length, GFP_KERNEL);
        if (buffer == NULL) {
            return -ENOMEM;
        }
        memcpy(buffer, s->draft, s->draft_length);
        kfree(s->draft);
        s->draft = buffer;
        s->draft_capacity = next_length;
    }
    memmove(buffer + meta_offset + writer.used, buffer + meta_offset + old_prefix_len,
            s->draft_length - (meta_offset + old_prefix_len));
    memcpy(buffer + meta_offset, header_and_name, writer.used);
    s->draft_length = next_length;
    return 0;
}

static int base_document(struct keyboard_config_service *s, const void *body, size_t length,
                         bool apply, bool dependencies)
{
    struct nlk_keyboard_ctx *keyboard = s->keyboard;
    struct kb_doc_reader reader, value;
    uint8_t seen[NLK_KB_MAX_LAYERS][16] = {{0}};
    uint8_t metadata_count = 0, header_count = 0;
    uint16_t tag, key_count = 0;
    int rc;

    kb_doc_reader_init(&reader, body, length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        if (tag == NLK_KB_TAG_METADATA) {
            if (metadata_read(&value, &metadata_count)) {
                return -EINVAL;
            }
        } else if (tag == NLK_KB_TAG_BASE_HEADER) {
            struct nlk_kb_params parameters = keyboard->params;
            char layout[32];
            kb_doc_string(&value, layout, sizeof(layout));
            if (strcmp(layout, kb_selected_layout.id) ||
                READ_UINT(&value, 2) != kb_selected_layout.version || ++header_count != 1) {
                return -EINVAL;
            }
            parameters.default_layer = READ_UINT(&value, 1);
            parameters.debounce_ms = READ_UINT(&value, 1);
            parameters.poll_rate_hz = READ_UINT(&value, 2);
            parameters.nkro_enable = READ_UINT(&value, 1);
            parameters.win_lock = READ_UINT(&value, 1);
            parameters.fn_lock = READ_UINT(&value, 1);
            parameters.sleep_timeout_min = READ_UINT(&value, 1);
            if (parameters.default_layer >= keyboard->desc.layer_count ||
                parameters.debounce_ms < 1 || parameters.debounce_ms > 100 ||
                parameters.poll_rate_hz < 1 || parameters.poll_rate_hz > 1000 ||
                parameters.nkro_enable > 1 || parameters.win_lock > 1 || parameters.fn_lock > 1) {
                return -EINVAL;
            }
            if (apply) {
                keyboard->default_layer = parameters.default_layer;
                nlk_cap_keyboard_set_params(keyboard, &parameters);
            }
        } else if (tag == NLK_KB_TAG_BASE_KEY) {
            uint8_t layer = READ_UINT(&value, 1);
            uint16_t key = READ_UINT(&value, 2);
            uint32_t code = READ_UINT(&value, 4);
            uint8_t binding_count = READ_UINT(&value, 1);
            if (layer >= keyboard->desc.layer_count || key >= keyboard->desc.key_count ||
                code > 255 || binding_count > NLK_KB_MAX_KEY_ACTIONS ||
                (seen[layer][key / 8] & (1U << (key % 8)))) {
                return -EINVAL;
            }
            seen[layer][key / 8] |= 1U << (key % 8);
            key_count++;
            if (apply) {
                keyboard->keymap[layer][key] = code;
                keyboard->action_chains[layer][key].count = 0;
            }
            for (uint8_t i = 0; i < binding_count; i++) {
                char id[32];
                kb_doc_string(&value, id, sizeof(id));
                uint16_t argument_length = READ_UINT(&value, 2);
                const uint8_t *arguments = kb_doc_bytes(&value, argument_length);
                const struct kb_action_definition *definition = find_action(id);
                if (arguments == NULL || definition == NULL) {
                    return -EINVAL;
                }
                rc = action_arguments(definition, arguments, argument_length,
                                      dependencies ? &s->store : NULL, 0, NULL);
                if (rc) {
                    return rc;
                }
                if (apply) {
                    uint8_t index;
                    struct nlk_kb_key_action_chain *chain = &keyboard->action_chains[layer][key];
                    for (index = 0; index < keyboard->action_count; index++) {
                        if (!strcmp(keyboard->action_names[index], id)) {
                            break;
                        }
                    }
                    chain->action_ids[chain->count++] = nlk_cap_keyboard_action_id(keyboard, index);
                }
            }
        } else if (tag >= NLK_KB_TAG_LAYOUT_EXTENSION_FIRST &&
                   tag <= NLK_KB_TAG_LAYOUT_EXTENSION_LAST) {
            if (kb_selected_layout.validate_record == NULL ||
                kb_selected_layout.validate_record(tag, value.data, value.left)) {
                return -EINVAL;
            }
            if (apply && kb_selected_layout.apply_record != NULL) {
                kb_selected_layout.apply_record(tag, value.data, value.left);
            }
            value.left = 0;
        } else {
            return -ENOTSUP;
        }
        if (value.error || value.left) {
            return -EINVAL;
        }
    }
    if (rc < 0 || metadata_count != 1 || header_count != 1 ||
        key_count != keyboard->desc.key_count * keyboard->desc.layer_count) {
        return -EINVAL;
    }
    return 0;
}

static int light_document(struct keyboard_config_service *s, const void *body, size_t length,
                          bool apply)
{
    struct nlk_keyboard_ctx *keyboard = s->keyboard;
    struct nlk_kb_light_layer layers[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    struct kb_effect_parameters parameters[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS] = {{0}};
    struct kb_doc_reader reader, value;
    uint8_t metadata_count = 0, count = 0;
    uint16_t tag;
    int rc;

    kb_doc_reader_init(&reader, body, length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        if (tag == NLK_KB_TAG_METADATA) {
            if (metadata_read(&value, &metadata_count)) {
                return -EINVAL;
            }
        } else if (tag == NLK_KB_TAG_LIGHT_LAYER) {
            char trigger[32];
            if (count >= CONFIG_KEYBOARD_LIGHT_MAX_LAYERS) {
                return -ENOSPC;
            }
            struct nlk_kb_light_layer *layer = &layers[count];
            memset(layer, 0, sizeof(*layer));
            layer->x = READ_UINT(&value, 1);
            layer->y = READ_UINT(&value, 1);
            layer->width = READ_UINT(&value, 1);
            layer->height = READ_UINT(&value, 1);
            layer->z_order = (int16_t)READ_UINT(&value, 2);
            layer->opacity = READ_UINT(&value, 1);
            layer->activation = READ_UINT(&value, 1);
            kb_doc_string(&value, trigger, sizeof(trigger));
            if (layer->width == 0 || layer->height == 0 ||
                layer->x + layer->width > kb_selected_layout.cols ||
                layer->y + layer->height > kb_selected_layout.rows ||
                layer->activation > NLK_KB_LIGHT_PULSE ||
                (trigger[0] && find_action(trigger) == NULL) ||
                ((layer->activation == 0) != (trigger[0] == 0)) ||
                style_read(&value, &layer->cfg, &parameters[count])) {
                return -EINVAL;
            }
            layer->trigger = trigger[0] ? nlk_fnv1a_24(trigger) : 0;
            layer->zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG;
            if (nlk_cap_keyboard_validate_light_layer(keyboard, layer)) {
                return -EINVAL;
            }
            count++;
        } else {
            return -ENOTSUP;
        }
        if (value.error || value.left) {
            return -EINVAL;
        }
    }
    if (rc < 0 || metadata_count != 1) {
        return -EINVAL;
    }
    if (apply) {
        if (keyboard->light_runtime != NULL) {
            memcpy(keyboard->light_runtime->parameters, parameters, sizeof(parameters));
        }
        keyboard->light_cfg.enable = s->selection.light_enabled;
        nlk_cap_keyboard_set_light_layers(keyboard, layers, count);
        if (keyboard->light_cb != NULL) {
            keyboard->light_cb(&keyboard->light_cfg, NULL, keyboard->indicator_leds,
                               keyboard->light_priv);
        }
    }
    return 0;
}

static int macro_document(struct keyboard_config_service *s, const void *body, size_t length,
                          bool apply)
{
    struct nlk_kb_macro_step steps[NLK_KB_MAX_MACRO_STEPS];
    struct kb_doc_reader reader, value;
    uint8_t metadata_count = 0, count = 0;
    uint32_t total_delay = 0;
    uint16_t tag;
    int rc;

    kb_doc_reader_init(&reader, body, length);
    while ((rc = kb_doc_next(&reader, &tag, &value)) > 0) {
        if (tag == NLK_KB_TAG_METADATA) {
            if (metadata_read(&value, &metadata_count)) {
                return -EINVAL;
            }
        } else if (tag == NLK_KB_TAG_MACRO_STEP) {
            if (count >= NLK_KB_MAX_MACRO_STEPS) {
                return -ENOSPC;
            }
            struct nlk_kb_macro_step *step = &steps[count++];
            step->type = READ_UINT(&value, 1);
            step->key_code = READ_UINT(&value, 1);
            step->delay_ms = READ_UINT(&value, 2);
            total_delay += step->delay_ms;
            if (total_delay > 600000 || step->type < 1 || step->type > 4 ||
                (step->type != 4 && (step->key_code == 0 || step->key_code > 0xe7))) {
                return -EINVAL;
            }
        } else {
            return -ENOTSUP;
        }
        if (value.error || value.left) {
            return -EINVAL;
        }
    }
    if (rc < 0 || metadata_count != 1) {
        return -EINVAL;
    }
    return apply ? nlk_cap_keyboard_set_macro(s->keyboard, 0, "playback", steps, count, 1) : 0;
}

/* Apply follows a side-effect-free validation pass over exactly the same bytes. */
int kb_config_document(struct keyboard_config_service *s, uint8_t kind, const void *body,
                    size_t length, bool apply, bool dependencies)
{
    switch (kind) {
    case KB_STORE_BASE:
        return base_document(s, body, length, apply, dependencies);
    case KB_STORE_LIGHT:
        return light_document(s, body, length, apply);
    case KB_STORE_MACRO:
        return macro_document(s, body, length, apply);
    default:
        return -EINVAL;
    }
}

int kb_config_definition(uint8_t resource, const char *id, struct kb_doc_writer *w)
{
    size_t rec, i;
    const struct kb_action_definition *a = resource == NLK_KB_RESOURCE_ACTION ? find_action(id) : NULL;
    const struct kb_effect_definition *e = resource == NLK_KB_RESOURCE_EFFECT ? find_effect(id) : NULL;
    if (resource == NLK_KB_RESOURCE_LAYOUT) {
        const struct kb_layout_definition *l = &kb_selected_layout;
        if (id[0] && strcmp(id, l->id)) {
            return -ENOENT;
        }
        kb_config_metadata(w, l->id, "Compiled physical layout");
        rec = kb_doc_record(w, NLK_KB_TAG_LAYOUT);
        kb_doc_str(w, l->id);
        kb_doc_put(w, l->version, 2);
        kb_doc_put(w, l->layer_count, 1);
        kb_doc_put(w, l->rows, 1);
        kb_doc_put(w, l->cols, 1);
        kb_doc_end(w, rec);
        for (i = 0; i < l->key_count; i++) {
            const struct kb_key_geometry *k = &l->keys[i];
            rec = kb_doc_record(w, NLK_KB_TAG_LAYOUT_KEY);
            kb_doc_put(w, i, 2);
            kb_doc_put(w, k->x, 2);
            kb_doc_put(w, k->y, 2);
            kb_doc_put(w, k->width, 2);
            kb_doc_put(w, k->height, 2);
            kb_doc_put(w, k->row, 1);
            kb_doc_put(w, k->col, 1);
            kb_doc_put(w, k->led, 1);
            kb_doc_end(w, rec);
        }
    } else {
        if (a == NULL && e == NULL) {
            return -ENOENT;
        }
        kb_config_metadata(w, id, a != NULL ? a->description : e->description);
        for (i = 1; e != NULL && i <= 3; i++) {
            if (!(e->fields & (1U << (i - 1)))) {
                continue;
            }
            rec = kb_doc_record(w, NLK_KB_TAG_PARAMETER);
            kb_doc_put(w, i, 2);
            kb_doc_put(w, i == 3 ? 2 : 1, 1);
            kb_doc_put(w, 0, 2);
            kb_doc_put(w, 255, 2);
            kb_doc_put(w, i == 1 ? 200 : 128, 2);
            kb_doc_str(w, i == 1 ? "brightness" : (i == 2 ? "speed" : "color"));
            kb_doc_end(w, rec);
        }
    }
    {
        size_t count =
            a ? a->parameter_count : (e ? e->parameter_count : kb_selected_layout.parameter_count);
        const struct kb_parameter_definition *parameters =
            a ? a->parameters : (e ? e->parameters : kb_selected_layout.parameters);
        for (i = 0; i < count; i++) {
            const struct kb_parameter_definition *p = &parameters[i];
            rec = kb_doc_record(w, NLK_KB_TAG_PARAMETER);
            kb_doc_put(w, p->id, 2);
            kb_doc_put(w, p->type, 1);
            kb_doc_put(w, p->minimum, 2);
            kb_doc_put(w, p->maximum, 2);
            kb_doc_put(w, p->default_value, 2);
            kb_doc_str(w, p->name);
            kb_doc_end(w, rec);
        }
    }
    return w->error ? w->error : (int)w->used;
}

bool kb_config_referenced(const void *body, size_t length, uint8_t kind, const char *name)
{
    struct kb_doc_reader r, v;
    uint16_t tag;
    kb_doc_reader_init(&r, body, length);
    while (kb_doc_next(&r, &tag, &v) > 0) {
        if (tag == NLK_KB_TAG_BASE_KEY) {
            uint8_t count, i;
            (void)READ_UINT(&v, 1);
            (void)READ_UINT(&v, 2);
            (void)READ_UINT(&v, 4);
            count = READ_UINT(&v, 1);
            for (i = 0; i < count; i++) {
                char action_id[32];
                size_t len;
                const uint8_t *p;
                const struct kb_action_definition *def;
                kb_doc_string(&v, action_id, sizeof(action_id));
                len = READ_UINT(&v, 2);
                p = kb_doc_bytes(&v, len);
                def = find_action(action_id);
                if (p == NULL || def == NULL) {
                    return true; /* 引用损坏时拒绝删除 */
                }
                if (action_arguments(def, p, len, NULL, kind, name) != 0) {
                    return true;
                }
            }
        }
    }
    return r.error != 0;
}
