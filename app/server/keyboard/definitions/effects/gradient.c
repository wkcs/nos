/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_gradient(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                               uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)priv;
    nk60_effect_spectrum(phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE - (uint32_t)y * 1024U, rgb);
}

const struct kb_effect_definition kb_effect_gradient = {
    .id = "gradient",
    .description = "Diagonal spectrum",
    .cycle_order = 7,
    .engine_id = 6U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED,
    .render = kb_render_gradient,
};
