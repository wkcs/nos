/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * NK60 lighting renderers and their registration order.
 */
#ifndef NK60_LIGHT_EFFECTS_H
#define NK60_LIGHT_EFFECTS_H

#include <nos/keyboard.h>

/* The order defines IDs for this device's default configuration. */
enum nk60_light_effect_id {
    NK60_EFFECT_OFF,
    NK60_EFFECT_STATIC,
    NK60_EFFECT_BREATH,
    NK60_EFFECT_RAINBOW,
};

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
    uint16_t level = (uint16_t)((phase / 64U) % 510U);
    uint8_t i;

    if (level > 255U) {
        level = 510U - level;
    }
    nk60_effect_static(cfg, x, y, phase, rgb, priv);
    for (i = 0U; i < 3U; i++) {
        rgb[i] = (uint8_t)(((uint16_t)rgb[i] * level) / 255U);
    }
}

static void nk60_effect_rainbow(const struct nlk_kb_light_cfg *cfg, uint8_t x,
                                uint8_t y, uint32_t phase, uint8_t rgb[3], void *priv)
{
    uint16_t hue = (uint16_t)(((phase / 64U) + (uint32_t)x * 36U) % 768U);
    uint8_t ramp = (uint8_t)(hue % 256U);
    (void)cfg;
    (void)y;
    (void)priv;
    if (hue < 256U) {
        rgb[0] = (uint8_t)(255U - ramp);
        rgb[1] = ramp;
        rgb[2] = 0U;
    } else if (hue < 512U) {
        rgb[0] = 0U;
        rgb[1] = (uint8_t)(255U - ramp);
        rgb[2] = ramp;
    } else {
        rgb[0] = ramp;
        rgb[1] = 0U;
        rgb[2] = (uint8_t)(255U - ramp);
    }
}

static const struct keyboard_light_effect g_nk60_light_effects[] = {
    { "off", "Lights off", nk60_effect_off },
    { "static", "Solid color", nk60_effect_static },
    { "breath", "Breathing color", nk60_effect_breath },
    { "rainbow", "Rainbow wave", nk60_effect_rainbow },
};

#endif
