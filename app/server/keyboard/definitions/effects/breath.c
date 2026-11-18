/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_breath(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                             uint32_t phase, uint8_t rgb[3], void *priv)
{
    const uint32_t unity = 32768U;
    uint32_t position = phase % NK60_BREATH_PERIOD;
    uint32_t level;
    uint64_t eased;
    uint8_t i;

    if (position > NK60_BREATH_PERIOD / 2U) {
        position = NK60_BREATH_PERIOD - position;
    }
    /* Q15 smoothstep gives equal rise/fall times and gentle dark/bright turnarounds. */
    level = position * unity / (NK60_BREATH_PERIOD / 2U);
    /* Round only after the cubic; early truncation can introduce one-level reversals. */
    eased = (uint64_t)level * level * (3U * unity - 2U * level);
    level = (uint32_t)((eased + (1ULL << 29U)) >> 30U);
    /* Approximate gamma 2: linear LED power otherwise looks abrupt near darkness. */
    level = (level * level + unity / 2U) / unity;
    nk60_effect_color(cfg, x, y, phase, rgb, priv);
    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)(((uint32_t)rgb[i] * level + unity / 2U) / unity);
    }
}

const struct kb_effect_definition kb_effect_breath = {
    .id = "breath",
    .description = "Breathing color",
    .cycle_order = 2,
    .engine_id = 1U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_breath,
};
