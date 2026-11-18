/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_KEYBOARD_EFFECT_COMMON_H
#define NOS_KEYBOARD_EFFECT_COMMON_H
#include <nos/keyboard_definitions.h>
#define NK60_EFFECT_PERIOD 8192U
#define NK60_EFFECT_COLUMN_PHASE 512U
#define NK60_RAINBOW_COLUMN_PHASE 256U
#define NK60_BREATH_PERIOD 32768U

static inline void nk60_effect_color(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
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

static inline void nk60_effect_spectrum(uint32_t phase, uint8_t rgb[3])
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

static inline uint8_t nk60_effect_envelope(uint32_t phase)
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

static inline void nk60_effect_scale(uint8_t rgb[3], uint8_t level)
{
    uint8_t i;

    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)((uint16_t)rgb[i] * level / 255U);
    }
}

#endif
