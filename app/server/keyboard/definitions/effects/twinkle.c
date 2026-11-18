/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_twinkle(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                              uint32_t phase, uint8_t rgb[3], void *priv)
{
    uint32_t seed = (uint32_t)x + (uint32_t)y * 14U + 1U;
    uint32_t position;
    uint8_t level = 0U;

    (void)cfg;
    (void)priv;
    /* Stable per-pixel hashing gives independent stars without mutable random state. */
    seed = (seed ^ (seed >> 16U)) * 0x45d9f3bU;
    seed = (seed ^ (seed >> 16U)) * 0x45d9f3bU;
    seed ^= seed >> 16U;
    position = (phase * (1U + (seed & 3U)) + (seed >> 2U)) % NK60_EFFECT_PERIOD;
    if (position < NK60_EFFECT_PERIOD / 4U) {
        level = nk60_effect_envelope(position * 4U);
    }
    nk60_effect_spectrum(seed >> 8U, rgb);
    nk60_effect_scale(rgb, level);
}

const struct kb_effect_definition kb_effect_twinkle = {
    .id = "twinkle",
    .description = "Twinkling stars",
    .cycle_order = 9,
    .engine_id = 8U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED,
    .render = kb_render_twinkle,
};
