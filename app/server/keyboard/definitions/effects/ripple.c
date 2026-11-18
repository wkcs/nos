/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_ripple(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                             uint32_t phase, uint8_t rgb[3], void *priv)
{
    int dx = (int)x * 2 - 13;
    int dy = (int)y * 2 - 4;
    uint32_t major;
    uint32_t minor;

    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    major = (uint32_t)(dx > dy ? dx : dy);
    minor = (uint32_t)(dx > dy ? dy : dx);
    /* Octagonal distance approximates round rings without a per-pixel square root. */
    nk60_effect_color(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, nk60_effect_envelope(phase - major * 256U - minor * 96U));
}

const struct kb_effect_definition kb_effect_ripple = {
    .id = "ripple",
    .description = "Center ripple",
    .cycle_order = 8,
    .engine_id = 7U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_ripple,
};
