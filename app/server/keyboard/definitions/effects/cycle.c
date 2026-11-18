/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_cycle(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                            uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)x;
    (void)y;
    (void)priv;
    nk60_effect_spectrum(phase, rgb);
}

const struct kb_effect_definition kb_effect_cycle = {
    .id = "cycle",
    .description = "Spectrum cycle",
    .cycle_order = 4,
    .engine_id = 3U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED,
    .render = kb_render_cycle,
};
