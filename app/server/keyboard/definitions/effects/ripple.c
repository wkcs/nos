/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "common.h"

static void render_ripple_at(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                             int cx, int cy, uint32_t phase, uint8_t rgb[3], void *priv)
{
    int dx = (int)x * 2 - cx;
    int dy = (int)y * 2 - cy;
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

static void kb_render_ripple(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                             uint32_t phase, uint8_t rgb[3], void *priv)
{
    render_ripple_at(cfg, x, y, 13, 4, phase, rgb, priv);
}

/* 当动作触发提供了键坐标时以触发按键为圆心扩散，否则回退为键盘中心 */
static void render_configured(const struct kb_effect_render_context *ctx, uint8_t rgb[3],
                              void *state)
{
    int cx = 13;
    int cy = 4;

    /* 仅当动作提供了有效键坐标时以触发按键为圆心扩散，否则回退为键盘几何中心 */
    if (ctx->trigger != NULL && ctx->trigger->event != KB_LIGHT_TRIG_NONE &&
        ctx->trigger->key_x != KB_LIGHT_COORD_NONE &&
        ctx->trigger->key_y != KB_LIGHT_COORD_NONE) {
        cx = (int)ctx->trigger->key_x * 2;
        cy = (int)ctx->trigger->key_y * 2;
    }
    render_ripple_at(ctx->cfg, ctx->x, ctx->y, cx, cy, ctx->phase, rgb, state);
}

const struct kb_effect_definition kb_effect_ripple = {
    .id = "ripple",
    .description = "Center ripple",
    .cycle_order = 8,
    .engine_id = 7U,
    .fields = KB_EFFECT_FIELD_BRIGHTNESS | KB_EFFECT_FIELD_SPEED | KB_EFFECT_FIELD_COLOR,
    .render = kb_render_ripple,
    .render_configured = render_configured,
};
