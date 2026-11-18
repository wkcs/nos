/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_wave(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y, uint32_t phase,
                           uint8_t rgb[3], void *priv)
{
    nk60_effect_color(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, nk60_effect_envelope(phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE));
}

const struct kb_effect_definition kb_effect_wave = {
    .id = "wave",
    .description = "Flowing color",
    .cycle_order = 5,
    .engine_id = 4U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_wave,
};
