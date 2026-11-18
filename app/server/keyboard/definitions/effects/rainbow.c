/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_rainbow(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                              uint32_t phase, uint8_t rgb[3], void *priv)
{
    /* Mixed-channel anchors add warm, mint, sky and rose tones between primary hues. */
    static const uint8_t palette[16][3] = {
        {255U, 16U, 32U},  {240U, 48U, 16U},  {255U, 112U, 24U}, {240U, 192U, 16U},
        {224U, 240U, 32U}, {144U, 255U, 16U}, {64U, 240U, 48U},  {16U, 224U, 128U},
        {32U, 240U, 224U}, {16U, 160U, 255U}, {32U, 80U, 240U},  {96U, 16U, 255U},
        {176U, 32U, 240U}, {240U, 16U, 224U}, {255U, 32U, 144U}, {240U, 16U, 64U},
    };
    uint32_t position = (phase - (uint32_t)x * NK60_RAINBOW_COLUMN_PHASE) % NK60_EFFECT_PERIOD;
    uint32_t index = position / 512U;
    uint32_t fraction = position % 512U;
    uint8_t i;

    (void)cfg;
    (void)y;
    (void)priv;
    /* Interpolate the full phase before rounding; twice-wider bands soften key-to-key steps. */
    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)(((uint32_t)palette[index][i] * (512U - fraction) +
                            (uint32_t)palette[(index + 1U) % 16U][i] * fraction + 256U) /
                           512U);
    }
}

const struct kb_effect_definition kb_effect_rainbow = {
    .id = "rainbow",
    .description = "Rainbow wave",
    .cycle_order = 3,
    .engine_id = 2U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED,
    .render = kb_render_rainbow,
};
