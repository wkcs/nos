/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * NK60 lighting renderers and their registration order.
 */
#ifndef NK60_LIGHT_EFFECTS_H
#define NK60_LIGHT_EFFECTS_H

#include <nos/keyboard.h>

/* Phase advances by speed + 1 per 30 ms; powers of two also wrap at UINT32_MAX. */
#define NK60_EFFECT_PERIOD 8192U
#define NK60_EFFECT_COLUMN_PHASE 512U
#define NK60_RAINBOW_COLUMN_PHASE 256U
#define NK60_BREATH_PERIOD 32768U

/* The order defines IDs for this device's default configuration. */
enum nk60_light_effect_id {
    NK60_EFFECT_OFF,
    NK60_EFFECT_STATIC,
    NK60_EFFECT_BREATH,
    NK60_EFFECT_RAINBOW,
    NK60_EFFECT_CYCLE,
    NK60_EFFECT_WAVE,
    NK60_EFFECT_COMET,
    NK60_EFFECT_GRADIENT,
    NK60_EFFECT_RIPPLE,
    NK60_EFFECT_TWINKLE,
    NK60_EFFECT_COUNT,
};

#define NK60_EFFECT_MASK ((1U << NK60_EFFECT_COUNT) - 1U)
_Static_assert(NK60_EFFECT_COUNT <= 16U, "NK60 effects must fit the zone bitmask");

static void nk60_effect_off(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                            uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)x;
    (void)y;
    (void)phase;
    (void)priv;
    rgb[0] = 0U;
    rgb[1] = 0U;
    rgb[2] = 0U;
}

static void nk60_effect_static(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                               uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)x;
    (void)y;
    (void)phase;
    (void)priv;
    rgb[0] = cfg->color_r;
    rgb[1] = cfg->color_g;
    rgb[2] = cfg->color_b;
}

static void nk60_effect_breath(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                               uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
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
    nk60_effect_static(cfg, x, y, phase, rgb, priv);
    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)(((uint32_t)rgb[i] * level + unity / 2U) / unity);
    }
}

static void nk60_effect_spectrum(uint32_t phase, uint8_t rgb[3])
{
    uint16_t hue = (uint16_t)((phase % NK60_EFFECT_PERIOD) * 1536U / NK60_EFFECT_PERIOD);
    uint8_t ramp = (uint8_t)(hue % 256U);

    switch (hue / 256U) {
    case 0U:
        rgb[0] = 255U;
        rgb[1] = ramp;
        rgb[2] = 0U;
        break;
    case 1U:
        rgb[0] = 255U - ramp;
        rgb[1] = 255U;
        rgb[2] = 0U;
        break;
    case 2U:
        rgb[0] = 0U;
        rgb[1] = 255U;
        rgb[2] = ramp;
        break;
    case 3U:
        rgb[0] = 0U;
        rgb[1] = 255U - ramp;
        rgb[2] = 255U;
        break;
    case 4U:
        rgb[0] = ramp;
        rgb[1] = 0U;
        rgb[2] = 255U;
        break;
    default:
        rgb[0] = 255U;
        rgb[1] = 0U;
        rgb[2] = 255U - ramp;
        break;
    }
}

static void nk60_effect_rainbow(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                                uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    /* Mixed-channel anchors add warm, mint, sky and rose tones between primary hues. */
    static const uint8_t palette[16][3] = {
        { 255U, 16U, 32U }, { 240U, 48U, 16U },
        { 255U, 112U, 24U }, { 240U, 192U, 16U },
        { 224U, 240U, 32U }, { 144U, 255U, 16U },
        { 64U, 240U, 48U }, { 16U, 224U, 128U },
        { 32U, 240U, 224U }, { 16U, 160U, 255U },
        { 32U, 80U, 240U }, { 96U, 16U, 255U },
        { 176U, 32U, 240U }, { 240U, 16U, 224U },
        { 255U, 32U, 144U }, { 240U, 16U, 64U },
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
                            (uint32_t)palette[(index + 1U) % 16U][i] * fraction + 256U) / 512U);
    }
}

static uint8_t nk60_effect_envelope(uint32_t phase)
{
    uint32_t position = phase % NK60_EFFECT_PERIOD;
    uint32_t level;

    if (position > NK60_EFFECT_PERIOD / 2U) {
        position = NK60_EFFECT_PERIOD - position;
    }
    level = position * 255U / (NK60_EFFECT_PERIOD / 2U);
    /* Smoothstep flattens the dark/bright endpoints without a floating-point sine. */
    return (uint8_t)(level * level * (765U - 2U * level) / 65025U);
}

static void nk60_effect_scale(uint8_t rgb[3], uint8_t level)
{
    uint8_t i;

    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)((uint16_t)rgb[i] * level / 255U);
    }
}

static void nk60_effect_cycle(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                              uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)x;
    (void)y;
    (void)priv;
    nk60_effect_spectrum(phase, rgb);
}

static void nk60_effect_wave(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                             uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    nk60_effect_static(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, nk60_effect_envelope(phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE));
}

static void nk60_effect_comet(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                              uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    uint32_t distance = (phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE) % NK60_EFFECT_PERIOD;
    uint32_t level = 0U;

    /* A soft leading edge avoids a flash when the head enters the next column. */
    if (distance > NK60_EFFECT_PERIOD - 512U) {
        level = (distance - (NK60_EFFECT_PERIOD - 512U)) * 255U / 512U;
    } else if (distance < 3072U) {
        level = (3072U - distance) * 255U / 3072U;
    }
    nk60_effect_static(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, (uint8_t)(level * level / 255U));
}

static void nk60_effect_gradient(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                                 uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)priv;
    nk60_effect_spectrum(phase - (uint32_t)x * NK60_EFFECT_COLUMN_PHASE -
                         (uint32_t)y * 1024U, rgb);
}

static void nk60_effect_ripple(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                               uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
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
    nk60_effect_static(cfg, x, y, phase, rgb, priv);
    nk60_effect_scale(rgb, nk60_effect_envelope(phase - major * 256U - minor * 96U));
}

static void nk60_effect_twinkle(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                                uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
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

static const struct keyboard_light_effect g_nk60_light_effects[] = {
    { "off", "Lights off", nk60_effect_off },
    { "static", "Solid color", nk60_effect_static },
    { "breath", "Breathing color", nk60_effect_breath },
    { "rainbow", "Rainbow wave", nk60_effect_rainbow },
    { "cycle", "Spectrum cycle", nk60_effect_cycle },
    { "wave", "Flowing color", nk60_effect_wave },
    { "comet", "Comet trail", nk60_effect_comet },
    { "gradient", "Diagonal spectrum", nk60_effect_gradient },
    { "ripple", "Center ripple", nk60_effect_ripple },
    { "twinkle", "Twinkling stars", nk60_effect_twinkle },
};

_Static_assert(sizeof(g_nk60_light_effects) / sizeof(g_nk60_light_effects[0]) ==
               NK60_EFFECT_COUNT, "NK60 effect IDs must match registration order");

#endif
