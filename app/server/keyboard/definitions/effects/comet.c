/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void kb_render_comet(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                            uint32_t phase, uint8_t rgb[3], void *priv)
{
    uint32_t distance = (phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE) % NK60_EFFECT_PERIOD;
    uint32_t level = 0U;

    /* A soft leading edge avoids a flash when the head enters the next column. */
    if (distance > NK60_EFFECT_PERIOD - 512U) {
        level = (distance - (NK60_EFFECT_PERIOD - 512U)) * 255U / 512U;
    } else if (distance < 3072U) {
        level = (3072U - distance) * 255U / 3072U;
    }
    nk60_effect_color(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, (uint8_t)(level * level / 255U));
}

static const struct kb_parameter_definition parameters[] = {
    {.id = 10, .type = 1, .minimum = 1, .maximum = 30, .default_value = 12, .name = "trail_length"},
};
static void render_configured(const struct kb_effect_render_context *ctx, uint8_t rgb[3],
                              void *state)
{
    uint32_t distance = (ctx->phase - (uint32_t)ctx->x * NK60_EFFECT_COLUMN_PHASE) % NK60_EFFECT_PERIOD;
    uint32_t trail = (uint32_t)kb_effect_parameter_u8(ctx->parameters, 10, 12) * 256;
    uint32_t level = 0;
    (void)state;
    if (distance > NK60_EFFECT_PERIOD - 512U) {
        level = (distance - (NK60_EFFECT_PERIOD - 512U)) * 255U / 512U;
    } else if (distance < trail) {
        level = (trail - distance) * 255U / trail;
    }
    nk60_effect_color(ctx->cfg, ctx->x, ctx->y, ctx->phase, rgb, NULL);
    nk60_effect_scale(rgb, (uint8_t)(level * level / 255U));
}

const struct kb_effect_definition kb_effect_comet = {
    .parameters = parameters,
    .parameter_count = 1,
    .render_configured = render_configured,
    .id = "comet",
    .description = "Comet trail",
    .cycle_order = 6,
    .engine_id = 5U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_comet,
};
