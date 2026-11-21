/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include <nos/keyboard_definitions.h>
#include <nos/keyboard_object_store.h>

struct request_probe {
    unsigned int count;
    enum kb_action_request request;
    char target[KB_STORE_NAME_SIZE];
};

static int capture_request(void *context, enum kb_action_request request, const char *target)
{
    struct request_probe *probe = context;

    probe->count++;
    probe->request = request;
    strcpy(probe->target, target != NULL ? target : "");
    return 0;
}

static void test_action_callbacks(void)
{
    const struct {
        const char *id;
        enum kb_action_request request;
        bool target, pass;
    } cases[] = {
        {"config_select", KB_ACTION_REQUEST_SELECT_BASE, true, false},
        {"config_next", KB_ACTION_REQUEST_BASE_NEXT, false, false},
        {"config_prev", KB_ACTION_REQUEST_BASE_PREV, false, false},
        {"light_next", KB_ACTION_REQUEST_LIGHT_NEXT, false, false},
        {"light_prev", KB_ACTION_REQUEST_LIGHT_PREV, false, false},
        {"light_toggle", KB_ACTION_REQUEST_LIGHT_TOGGLE, false, true},
        {"macro_play", KB_ACTION_REQUEST_MACRO_PLAY, true, false},
        {"macro_stop", KB_ACTION_REQUEST_MACRO_STOP, false, false},
    };
    size_t i, j;
    struct request_probe probe = {0};
    struct kb_action_runtime runtime = {.context = &probe, .request = capture_request};
    struct key_action_context event = {.key_code = 4, .initial_code = 4, .event_type = 1};
    uint8_t target[] = {4, 'w', 'o', 'r', 'k'};

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (j = 0; j < kb_action_definition_count; j++) {
            if (!strcmp(cases[i].id, kb_action_definitions[j]->id)) {
                break;
            }
        }
        assert(j < kb_action_definition_count);
        const struct kb_action_definition *def = kb_action_definitions[j];
        unsigned int before = probe.count;
        event.event_type = 0;
        assert(def->execute(&event, cases[i].target ? target : NULL,
                            cases[i].target ? sizeof(target) : 0,
                            &runtime) == (cases[i].pass ? KEY_ACTION_PASS : KEY_ACTION_DROP));
        assert(probe.count == before);
        event.event_type = 1;
        assert(def->execute(&event, cases[i].target ? target : NULL,
                            cases[i].target ? sizeof(target) : 0,
                            &runtime) == (cases[i].pass ? KEY_ACTION_PASS : KEY_ACTION_DROP));
        assert(probe.count == before + 1 && probe.request == cases[i].request);
        assert(!strcmp(probe.target, cases[i].target ? "work" : ""));
    }
    for (i = 0; i < kb_action_definition_count; i++) {
        const struct kb_action_definition *def = kb_action_definitions[i];
        if (!strcmp(def->id, "key_chord")) {
            uint8_t modifiers = 8;
            assert(def->execute(&event, &modifiers, 1, &runtime) ==
                   (int)(8U << 16 | 4U << 8 | 0xafU));
            event.event_type = 0;
            assert(def->execute(&event, &modifiers, 1, &runtime) == KEY_ACTION_DROP);
            event.event_type = 1;
        } else if (!strcmp(def->id, "caps_lock") || !strcmp(def->id, "key_light") ||
                   !strcmp(def->id, "win_lock")) {
            unsigned int before = probe.count;
            assert(def->execute(&event, NULL, 0, &runtime) == KEY_ACTION_PASS);
            assert(probe.count == before);
        }
    }
}

int main(void)
{
    const struct kb_layout_definition *layout = &kb_selected_layout;
    const char *engine_ids[] = {"static", "breath",   "rainbow", "cycle",  "wave",
                                "comet",  "gradient", "ripple",  "twinkle"};
    const struct {
        const char *id;
        uint8_t parameter_type;
    } expected_actions[] = {
        {"key_chord", 1U},     {"caps_lock", 0U},    {"key_light", 0U},   {"win_lock", 0U},
        {"config_select", 3U}, {"config_next", 0U},  {"config_prev", 0U}, {"light_next", 0U},
        {"light_prev", 0U},    {"light_toggle", 0U}, {"macro_play", 4U},  {"macro_stop", 0U},
    };
    size_t i, j;
    bool matrix[5][14] = {{false}};
    bool leds[61] = {false};
    bool effects[16] = {false};

    assert(strcmp(layout->id, "ansi61") == 0);
    assert(layout->key_count == 61U && layout->layer_count == 3U);
    assert(layout->rows == 5U && layout->cols == 14U);
    assert(layout->default_keymap[0] == KC_ESC);
    assert(layout->default_keymap[61] == KC_GRV);
    assert(layout->default_keymap[122] == KC_GRV);
    for (i = 0U; i < layout->key_count; i++) {
        const struct kb_key_geometry *key = &layout->keys[i];
        assert(key->row < 5U && key->col < 14U && key->led < 61U);
        assert(!matrix[key->row][key->col] && !leds[key->led]);
        matrix[key->row][key->col] = true;
        leds[key->led] = true;
        assert(key->led == i);
        assert(key->height == 100U && key->width >= 100U);
        assert(key->x + key->width <= 1500U && key->y == key->row * 100U);
        for (j = 0U; j < i; j++) {
            const struct kb_key_geometry *other = &layout->keys[j];
            if (key->row == other->row) {
                assert(key->x >= other->x + other->width);
            }
        }
    }
    assert(layout->keys[13].width == 200U);
    assert(layout->keys[14].width == 150U);
    assert(layout->keys[28].width == 175U);
    assert(layout->keys[40].width == 225U);
    assert(layout->keys[41].width == 225U);
    assert(layout->keys[52].width == 275U);
    assert(layout->keys[56].width == 625U);
    assert(layout->keys[56].col == 6U && layout->keys[60].col == 13U);
    assert(kb_effect_definition_count == 9U);
    for (i = 0U; i < kb_effect_definition_count; i++) {
        const struct kb_effect_definition *effect = kb_effect_definitions[i];
        assert(effect->render != NULL && effect->engine_id < 9U);
        assert(strcmp(effect->id, engine_ids[effect->engine_id]) == 0);
        assert(!effects[effect->engine_id]);
        effects[effect->engine_id] = true;
        assert(strcmp(effect->id, "off") != 0);
        if (strcmp(effect->id, "static") == 0) {
            assert(effect->fields == 5U);
        } else if (strcmp(effect->id, "breath") == 0 || strcmp(effect->id, "wave") == 0 ||
                   strcmp(effect->id, "comet") == 0 || strcmp(effect->id, "ripple") == 0) {
            assert(effect->fields == 7U);
        } else {
            assert(effect->fields == 3U);
        }
        for (j = 0U; j < i; j++) {
            assert(strcmp(effect->id, kb_effect_definitions[j]->id));
        }
    }
    assert(kb_action_definition_count == 12U);
    for (i = 0U; i < kb_action_definition_count; i++) {
        assert(strlen(kb_action_definitions[i]->id) < 16U);
        for (j = 0U; j < i; j++) {
            assert(strcmp(kb_action_definitions[i]->id, kb_action_definitions[j]->id));
        }
    }
    for (i = 0U; i < sizeof(expected_actions) / sizeof(expected_actions[0]); i++) {
        bool found = false;

        for (j = 0U; j < kb_action_definition_count; j++) {
            const struct kb_action_definition *action = kb_action_definitions[j];

            if (strcmp(action->id, expected_actions[i].id) == 0) {
                assert(action->execute != NULL);
                assert(action->parameter_count == (expected_actions[i].parameter_type ? 1U : 0U));
                if (action->parameter_count) {
                    assert(action->parameters[0].type == expected_actions[i].parameter_type);
                }
                found = true;
            }
        }
        assert(found);
    }
    {
        const struct kb_effect_definition *comet = NULL;
        const struct kb_effect_definition *ripple = NULL;
        struct nlk_kb_light_cfg white = {.color_r = 255, .color_g = 255, .color_b = 255};
        struct kb_effect_parameters short_trail = {5, {10, 0, 1, 1, 1}},
                                    long_trail = {5, {10, 0, 1, 1, 30}},
                                    empty_params = {0};
        struct kb_effect_render_context rctx = {
            .cfg = &white,
            .parameters = &short_trail,
            .x = 0,
            .y = 0,
            .phase = 1000,
        };
        uint8_t a[3], b[3], again[3], state[KB_EFFECT_STATE_MAX] = {0};
        for (i = 0; i < kb_effect_definition_count; i++) {
            if (!strcmp(kb_effect_definitions[i]->id, "comet")) {
                comet = kb_effect_definitions[i];
            }
            if (!strcmp(kb_effect_definitions[i]->id, "ripple")) {
                ripple = kb_effect_definitions[i];
            }
        }
        assert(comet && comet->parameter_count == 1 && comet->parameters[0].id == 10);
        comet->render_configured(&rctx, a, state);
        rctx.parameters = &long_trail;
        comet->render_configured(&rctx, b, state);
        rctx.parameters = &short_trail;
        comet->render_configured(&rctx, again, state);
        assert(a[0] == 0 && b[0] > 0 && !memcmp(a, again, 3));

        assert(ripple && ripple->render_configured);
        struct kb_light_trigger_data origin_trig = {
            .event = KB_LIGHT_TRIG_KEY_DOWN,
            .key_x = 0,
            .key_y = 0,
        };
        struct kb_light_trigger_data empty_trig = {0};
        struct kb_effect_render_context ripple_ctx = {
            .cfg = &white,
            .parameters = &empty_params,
            .trigger = NULL,
            .x = 0,
            .y = 0,
            .phase = 0,
        };
        uint8_t center_px[3], origin_px[3], empty_px[3];
        ripple->render_configured(&ripple_ctx, center_px, state);
        ripple_ctx.trigger = &origin_trig;
        ripple->render_configured(&ripple_ctx, origin_px, state);
        ripple_ctx.trigger = &empty_trig;
        ripple->render_configured(&ripple_ctx, empty_px, state);
        /* 触发点 (0,0) 在 phase=0 时距离为 0 (最暗), 而默认中心 (13,4) 此时距离较大 */
        assert(origin_px[0] != center_px[0]);
        /* 零初始化无触发上下文与 NULL trigger 等价，均以键盘几何中心 (13,4) 扩散 */
        assert(memcmp(empty_px, center_px, sizeof(center_px)) == 0);
    }
    test_action_callbacks();
    return 0;
}
