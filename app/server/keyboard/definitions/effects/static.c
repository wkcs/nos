/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_static(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                             uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)x;
    (void)y;
    (void)phase;
    (void)priv;
    rgb[0] = cfg->color_r;
    rgb[1] = cfg->color_g;
    rgb[2] = cfg->color_b;
}

const struct kb_effect_definition kb_effect_static = {
    .id = "static",
    .description = "Solid color",
    .cycle_order = 1,
    .engine_id = 0U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_static,
};
