/**
 * Copyright (C) 2024-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/clk.h>
#include <kernel/init.h>
#include <nos/display.h>
#include <nos/keyboard_definitions.h>
#include <string.h>

/* Preserve effect timing while advancing at every display refresh. */
#define KEYBOARD_LIGHT_PHASE_MS 30U

static struct keyboard_light_state g_frame_state;
/* Per-frame workspace. Layer definitions and ordering come from the active configuration. */
static struct ds_render_layer g_frame_layers[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
static uint32_t g_phase[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
static uint8_t g_effect_state[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS][KB_EFFECT_STATE_MAX];
static uint8_t g_phase_remainder[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
static uint32_t g_last_trigger_seq[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
static bool g_pulse_finished[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];

static void keyboard_render_light_layer(struct ds_frame *frame, uint8_t opacity, void *ctx)
{
    const struct keyboard_light_layer_state *state = ctx;
    const struct nlk_kb_light_layer *layer = &state->layer;
    size_t index = state - g_frame_state.layers;
    uint16_t y;

    for (y = 0U; y < layer->height; y++) {
        uint16_t x;

        for (x = 0U; x < layer->width; x++) {
            uint8_t rgb[3];
            uint8_t pixel[DS_COLOR_DATA_MAX];

            if (state->definition && state->definition->render_configured) {
                const struct kb_effect_render_context render_ctx = {
                    .cfg = &layer->cfg,
                    .parameters = &state->parameters,
                    .trigger = &state->trigger,
                    .x = (uint8_t)(layer->x + x),
                    .y = (uint8_t)(layer->y + y),
                    .phase = g_phase[index],
                    .activation = layer->activation,
                };
                state->definition->render_configured(&render_ctx, rgb, g_effect_state[index]);
            } else {
                state->render(&layer->cfg, layer->x + x, layer->y + y, g_phase[index], rgb, state->priv);
            }
            pixel[DS_COLOR_R] = (uint8_t)((uint16_t)rgb[0] * layer->cfg.brightness / 255U);
            pixel[DS_COLOR_G] = (uint8_t)((uint16_t)rgb[1] * layer->cfg.brightness / 255U);
            pixel[DS_COLOR_B] = (uint8_t)((uint16_t)rgb[2] * layer->cfg.brightness / 255U);
            display_frame_blend_pixel(frame, layer->x + x, layer->y + y, pixel, opacity);
        }
    }
}

static const struct ds_render_layer *keyboard_prepare_light_layers(void *ctx, size_t *count)
{
    static uint32_t last_update_ms;
    static bool was_active[CONFIG_KEYBOARD_LIGHT_MAX_LAYERS];
    uint32_t generation = g_frame_state.generation;
    uint32_t now_ms = (uint32_t)tick_to_msec_64(cpu_run_ticks());
    uint32_t elapsed_ms = now_ms - last_update_ms;
    uint8_t i;

    (void)ctx;
    keyboard_server_get_light_state(&g_frame_state);
    last_update_ms = now_ms;
    *count = 0U;
    for (i = 0U; i < g_frame_state.count; i++) {
        struct keyboard_light_layer_state *state = &g_frame_state.layers[i];
        struct ds_render_layer draw = {
            .z_order = state->layer.z_order, .opacity = state->layer.opacity,
            .render = keyboard_render_light_layer, .ctx = state,
        };
        size_t pos = *count;
        bool pulse_restarted = state->layer.activation == NLK_KB_LIGHT_PULSE &&
                               state->trigger.seq != 0U &&
                               state->trigger.seq != g_last_trigger_seq[i];

        if (generation != g_frame_state.generation || !was_active[i] || pulse_restarted) {
            memset(g_effect_state[i], 0, sizeof(g_effect_state[i]));
            g_phase[i] = 0U;
            g_phase_remainder[i] = 0U;
            g_pulse_finished[i] = false;
        } else if (state->active && !g_pulse_finished[i]) {
            uint32_t speed = (uint32_t)state->layer.cfg.speed + 1U;
            uint32_t fraction = (elapsed_ms % KEYBOARD_LIGHT_PHASE_MS) * speed +
                                g_phase_remainder[i];

            /* Split whole/remainder milliseconds to avoid overflow after a long pause. */
            g_phase[i] += (elapsed_ms / KEYBOARD_LIGHT_PHASE_MS) * speed +
                          fraction / KEYBOARD_LIGHT_PHASE_MS;
            g_phase_remainder[i] = (uint8_t)(fraction % KEYBOARD_LIGHT_PHASE_MS);
            /* 脉冲模式下播放完一个完整效果周期后自动停止，等待下次动作触发 */
            if (state->layer.activation == NLK_KB_LIGHT_PULSE && g_phase[i] >= 8192U) {
                g_pulse_finished[i] = true;
            }
        }
        g_last_trigger_seq[i] = state->trigger.seq;
        was_active[i] = state->active;
        if (!g_frame_state.enabled || !state->active ||
            (state->layer.activation == NLK_KB_LIGHT_PULSE && g_pulse_finished[i]) ||
            !state->layer.cfg.enable || state->render == NULL) {
            continue;
        }
        while (pos > 0U && g_frame_layers[pos - 1U].z_order > draw.z_order) {
            g_frame_layers[pos] = g_frame_layers[pos - 1U];
            pos--;
        }
        g_frame_layers[pos] = draw;
        (*count)++;
    }
    for (; i < CONFIG_KEYBOARD_LIGHT_MAX_LAYERS; i++) {
        was_active[i] = false;
        g_pulse_finished[i] = false;
        g_last_trigger_seq[i] = 0U;
    }
    return g_frame_layers;
}

static const struct ds_layer_source g_layer_source = {
    .prepare = keyboard_prepare_light_layers,
};

static int nk60_display_server_init(void)
{
    return display_server_register_layers(CONFIG_LED_DEV, &g_layer_source);
}
SYS_INIT(nk60_display_server_init, INIT_LEVEL_APPLICATION);
