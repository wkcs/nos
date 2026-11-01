/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "../../board/nk60-v2/nk60_light_effects.h"

int main(void)
{
    struct nlk_kb_light_cfg cfg = { .color_r = 200U, .color_g = 100U, .color_b = 50U };
    uint8_t rgb[3];
    uint8_t other[3];

    nk60_effect_off(&cfg, 0U, 0U, 0U, rgb, NULL);
    assert(rgb[0] == 0U && rgb[1] == 0U && rgb[2] == 0U);
    nk60_effect_static(&cfg, 0U, 0U, 0U, rgb, NULL);
    assert(rgb[0] == 200U && rgb[1] == 100U && rgb[2] == 50U);
    nk60_effect_breath(&cfg, 0U, 0U, 0U, rgb, NULL);
    assert(rgb[0] == 0U && rgb[1] == 0U && rgb[2] == 0U);
    nk60_effect_breath(&cfg, 0U, 0U, 255U * 64U, rgb, NULL);
    assert(rgb[0] == 200U && rgb[1] == 100U && rgb[2] == 50U);
    nk60_effect_rainbow(&cfg, 0U, 0U, 0U, rgb, NULL);
    nk60_effect_rainbow(&cfg, 1U, 0U, 0U, other, NULL);
    assert(rgb[0] != other[0] || rgb[1] != other[1] || rgb[2] != other[2]);
    nk60_effect_rainbow(&cfg, 0U, 0U, 256U * 64U, other, NULL);
    assert(rgb[0] != other[0] || rgb[1] != other[1] || rgb[2] != other[2]);
    assert(g_nk60_light_effects[NK60_EFFECT_BREATH].render == nk60_effect_breath);
    return 0;
}
