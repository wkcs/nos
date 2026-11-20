/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Adapt compiled definitions to the keyboard engine's legacy profile interface.
 */
#include <nos/keyboard_definitions.h>
#include <uapi/nos/errno.h>

static void kb_definition_string(char *out, size_t capacity, const char *value)
{
    size_t i = 0U;

    while (i + 1U < capacity && value[i] != '\0') {
        out[i] = value[i];
        i++;
    }
    out[i] = '\0';
}

int kb_definitions_apply_profile(struct keyboard_profile *profile,
                                 struct keyboard_profile_layout *layout,
                                 struct keyboard_light_effect effects[KB_EFFECT_ENGINE_LIMIT])
{
    bool used[KB_EFFECT_ENGINE_LIMIT] = {false};
    uint8_t effect_count = 0U;
    size_t i;

    if (profile == NULL || layout == NULL || effects == NULL ||
        kb_action_definition_count > NLK_KB_MAX_ACTIONS) {
        return -EINVAL;
    }
    /* The legacy renderer assigns dense numeric IDs. Reject gaps before startup. */
    for (i = 0U; i < kb_effect_definition_count; i++) {
        const struct kb_effect_definition *effect = kb_effect_definitions[i];
        uint8_t id = effect->engine_id;

        if (id >= KB_EFFECT_ENGINE_LIMIT || used[id]) {
            return -EINVAL;
        }
        used[id] = true;
        effects[id].name = effect->id;
        effects[id].desc = effect->description;
        effects[id].render = effect->render;
        if (effect_count <= id) {
            effect_count = id + 1U;
        }
    }
    for (i = 0U; i < effect_count; i++) {
        if (!used[i]) {
            return -EINVAL;
        }
    }
    layout->info.layout_id = NLK_KB_LAYOUT_STD_60_ANSI;
    layout->info.key_count = kb_selected_layout.key_count;
    layout->info.matrix_rows = kb_selected_layout.rows;
    layout->info.matrix_cols = kb_selected_layout.cols;
    kb_definition_string(layout->info.name, sizeof(layout->info.name), kb_selected_layout.id);
    layout->default_keymap = kb_selected_layout.default_keymap;
    profile->rows = kb_selected_layout.rows;
    profile->cols = kb_selected_layout.cols;
    profile->key_count = kb_selected_layout.key_count;
    profile->layer_count = kb_selected_layout.layer_count;
    profile->default_keymap = kb_selected_layout.default_keymap;
    profile->layout_count = 1U;
    profile->layouts = layout;
    profile->light_effect_count = effect_count;
    profile->light_effects = effects;
    return 0;
}

uint8_t kb_effect_parameter_u8(const struct kb_effect_parameters *parameters, uint16_t id,
                               uint8_t fallback)
{
    size_t offset = 0;
    while (offset + 4 <= parameters->length) {
        const uint8_t *p = parameters->data + offset;
        uint16_t field = p[0] | ((uint16_t)p[1] << 8);
        if (p[3] > parameters->length - offset - 4) {
            break;
        }
        if (field == id && p[2] == 1 && p[3] == 1) {
            return p[4];
        }
        offset += 4 + p[3];
    }
    return fallback;
}

/* 读取动作透传参数中的单字节数值，缺失或越界时返回 fallback */
uint8_t kb_trigger_arg_u8(const struct kb_light_trigger_data *trigger, size_t offset,
                          uint8_t fallback)
{
    if (trigger == NULL || offset >= trigger->arg_length) {
        return fallback;
    }
    return trigger->arguments[offset];
}
