/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <nos/keyboard_definitions.h>
#define __NOS_INIT_H__
#define SYS_INIT(fn, level) static void *const test_init_ref __attribute__((unused)) = (void *)&fn
#ifdef TEST_NK60_V1
#include "../../board/nk60-v1/nk60_keyboard.c"
#else
#include "../../board/nk60-v2/nk60_keyboard.c"
#endif

static nlk_kb_effect_render_fn_t find_effect(const char *name);
#define nk60_effect_static find_effect("static")
#define nk60_effect_breath find_effect("breath")
#define nk60_effect_rainbow find_effect("rainbow")

void keyboard_server_register_profile(const struct keyboard_profile *profile)
{
    (void)profile;
}

static int compare_colors(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;

    return (a > b) - (a < b);
}

static void test_breath(void)
{
    const struct nlk_kb_light_cfg white = {.color_r = 255U, .color_g = 255U, .color_b = 255U};
    const struct nlk_kb_light_cfg color = {.color_r = 200U, .color_g = 100U, .color_b = 0U};
    const uint8_t samples[] = {0U, 6U, 64U, 182U, 255U};
    uint8_t rgb[3];
    uint8_t other[3];
    uint8_t previous = 0U;
    uint32_t phase;

    /* A smooth perceptual envelope: equal time does not mean linear LED power. */
    for (phase = 0U; phase <= 16384U; phase += 4096U) {
        nk60_effect_breath(&white, 0U, 0U, phase, rgb, NULL);
        assert(rgb[0] == samples[phase / 4096U]);
    }
    for (phase = 0U; phase <= 32768U; phase++) {
        nk60_effect_breath(&white, 0U, 0U, phase, rgb, NULL);
        nk60_effect_breath(&white, 13U, 4U, 32768U - phase, other, NULL);
        assert(memcmp(rgb, other, sizeof(rgb)) == 0);
        assert(rgb[0] == rgb[1] && rgb[1] == rgb[2]);
        if (phase <= 16384U) {
            assert(rgb[0] >= previous);
        } else {
            assert(rgb[0] <= previous);
        }
        previous = rgb[0];
        nk60_effect_breath(&white, 0U, 0U, phase + 32768U, other, NULL);
        assert(memcmp(rgb, other, sizeof(rgb)) == 0);
        /* Even at maximum speed, one 30 ms interval cannot jump over 11 levels. */
        nk60_effect_breath(&white, 0U, 0U, phase + 256U, other, NULL);
        assert(abs((int)rgb[0] - other[0]) <= 11);
        nk60_effect_breath(&color, 4U, 2U, phase, other, NULL);
        assert(other[0] <= 200U && other[1] <= 100U && other[2] == 0U);
        assert(abs((int)other[0] - (int)other[1] * 2) <= 1);
    }
    /* Gentle turnarounds, including the unsigned phase counter wrapping to zero. */
    nk60_effect_breath(&white, 0U, 0U, 256U, rgb, NULL);
    assert(rgb[0] == 0U);
    nk60_effect_breath(&white, 0U, 0U, 16384U - 256U, rgb, NULL);
    assert(rgb[0] >= 254U);
    nk60_effect_breath(&white, 0U, 0U, UINT32_MAX, rgb, NULL);
    assert(rgb[0] == 0U && rgb[1] == 0U && rgb[2] == 0U);
}

static void test_rainbow(void)
{
    const struct nlk_kb_light_cfg cfg = {0};
    const uint8_t samples[4][3] = {
        {255U, 16U, 32U},
        {224U, 240U, 32U},
        {32U, 240U, 224U},
        {176U, 32U, 240U},
    };
    uint32_t colors[8192];
    unsigned int distinct = 1U;
    uint8_t rgb[3];
    uint8_t other[3];
    uint32_t phase;
    uint8_t x;

    /* At maximum speed the period is 8192 / 256 * 30 ms = 960 ms. */
    nk60_effect_rainbow(&cfg, 0U, 0U, 0U, rgb, NULL);
    nk60_effect_rainbow(&cfg, 0U, 0U, 8192U, other, NULL);
    assert(memcmp(rgb, other, sizeof(rgb)) == 0);
    nk60_effect_rainbow(&cfg, 0U, 0U, 16U, other, NULL);
    assert(memcmp(rgb, other, sizeof(rgb)) != 0);
    for (x = 0U; x < 4U; x++) {
        phase = (uint32_t)x * 2048U;
        nk60_effect_rainbow(&cfg, 0U, 0U, phase, rgb, NULL);
        assert(memcmp(rgb, samples[x], sizeof(rgb)) == 0);
    }
    for (phase = 0U; phase < 8192U; phase += 83U) {
        for (x = 0U; x < 13U; x++) {
            nk60_effect_rainbow(&cfg, x, 0U, phase, rgb, NULL);
            /* A color at x reappears one column to its right later. */
            nk60_effect_rainbow(&cfg, x + 1U, 4U, phase + 256U, other, NULL);
            assert(memcmp(rgb, other, sizeof(rgb)) == 0);
            /* Wider bands halve the maximum difference between neighboring keys. */
            nk60_effect_rainbow(&cfg, x + 1U, 0U, phase, other, NULL);
            for (uint8_t channel = 0U; channel < 3U; channel++) {
                int delta = (int)rgb[channel] - other[channel];

                assert(delta >= -48 && delta <= 48);
            }
        }
    }
    /* No channel jumps by more than 48 even across a full 30 ms at max speed. */
    for (phase = 0U; phase < 8192U; phase++) {
        nk60_effect_rainbow(&cfg, 0U, 0U, phase, rgb, NULL);
        /* Soft intermediate tones mix all three channels, including warm/pastel hues. */
        assert(rgb[0] > 0U && rgb[1] > 0U && rgb[2] > 0U);
        colors[phase] = (uint32_t)rgb[0] << 16U | (uint32_t)rgb[1] << 8U | rgb[2];
        nk60_effect_rainbow(&cfg, 0U, 0U, phase + 256U, other, NULL);
        for (x = 0U; x < 3U; x++) {
            int delta = (int)rgb[x] - other[x];

            assert(delta >= -48 && delta <= 48);
        }
    }
    qsort(colors, 8192U, sizeof(colors[0]), compare_colors);
    for (phase = 1U; phase < 8192U; phase++) {
        distinct += colors[phase] != colors[phase - 1U];
    }
    assert(distinct > 1536U);
    nk60_effect_rainbow(&cfg, 0U, 0U, UINT32_MAX, rgb, NULL);
    nk60_effect_rainbow(&cfg, 0U, 0U, 0U, other, NULL);
    for (x = 0U; x < 3U; x++) {
        int delta = (int)rgb[x] - other[x];

        assert(delta >= -1 && delta <= 1);
    }
}

static nlk_kb_effect_render_fn_t find_effect(const char *name)
{
    uint8_t i;

    for (i = 0U; i < g_nk60_kb_profile.light_effect_count; i++) {
        if (strcmp(g_nk60_light_effects[i].name, name) == 0) {
            return g_nk60_light_effects[i].render;
        }
    }
    assert(!"missing NK60 effect");
    return NULL;
}

static void test_additional_effects(void)
{
    const char *names[] = {"cycle", "wave", "comet", "gradient", "ripple", "twinkle"};
    const struct nlk_kb_light_cfg cfg = {.color_r = 200U, .color_g = 100U, .color_b = 0U};
    const struct nlk_kb_light_cfg black = {0};
    uint8_t rgb[3];
    uint8_t other[3];
    uint8_t i;
    uint8_t j;

    assert(g_nk60_kb_profile.light_effect_count == 9U);
    assert(NK60_EFFECT_STATIC == 0 && NK60_EFFECT_BREATH == 1 && NK60_EFFECT_RAINBOW == 2);
    for (i = 0U; i < g_nk60_kb_profile.light_zone_count; i++) {
        assert(g_nk60_light_zones[i].supported_effects == 0x01ffU);
    }
    for (i = 0U; i < g_nk60_kb_profile.light_effect_count; i++) {
        const struct keyboard_light_effect *effect = &g_nk60_light_effects[i];

        assert(effect->render != NULL);
        assert(strcmp(effect->name, "off") != 0);
        assert(strlen(effect->name) < NLK_KB_LIGHT_EFFECT_NAME_MAX);
        assert(strlen(effect->desc) < NLK_KB_LIGHT_EFFECT_DESC_MAX);
        for (j = 0U; j < i; j++) {
            assert(strcmp(effect->name, g_nk60_light_effects[j].name) != 0);
        }
    }
    for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
        nlk_kb_effect_render_fn_t render = find_effect(names[i]);
        bool animated = false;
        bool spatial = false;
        uint32_t phase;
        uint8_t x;
        uint8_t y;

        for (phase = 0U; phase < 8192U; phase += 127U) {
            render(&cfg, 0U, 0U, phase, rgb, NULL);
            render(&cfg, 0U, 0U, phase + 256U, other, NULL);
            animated |= memcmp(rgb, other, sizeof(rgb)) != 0;
            for (y = 0U; y < 5U; y++) {
                for (x = 0U; x < 14U; x++) {
                    render(&cfg, x, y, phase, rgb, NULL);
                    render(&cfg, x, y, phase, other, NULL);
                    assert(memcmp(rgb, other, sizeof(rgb)) == 0);
                    render(&cfg, x, y, phase + 8192U, other, NULL);
                    assert(memcmp(rgb, other, sizeof(rgb)) == 0);
                    render(&cfg, x, y, phase + 1U, other, NULL);
                    for (j = 0U; j < 3U; j++) {
                        int delta = (int)rgb[j] - other[j];

                        assert(delta >= -4 && delta <= 4);
                    }
                    render(&cfg, 0U, 0U, phase, other, NULL);
                    spatial |= memcmp(rgb, other, sizeof(rgb)) != 0;
                    if (strcmp(names[i], "cycle") == 0) {
                        assert(memcmp(rgb, other, sizeof(rgb)) == 0);
                    }
                    if (strcmp(names[i], "wave") == 0 || strcmp(names[i], "comet") == 0 ||
                        strcmp(names[i], "ripple") == 0) {
                        assert(rgb[0] <= cfg.color_r && rgb[1] <= cfg.color_g && rgb[2] == 0U);
                        assert(rgb[0] >= rgb[1] * 2U && rgb[0] <= rgb[1] * 2U + 1U);
                        render(&black, x, y, phase, other, NULL);
                        assert(other[0] == 0U && other[1] == 0U && other[2] == 0U);
                    }
                }
            }
        }
        assert(animated);
        assert(spatial == (strcmp(names[i], "cycle") != 0));
        for (y = 0U; y < 5U; y++) {
            for (x = 0U; x < 14U; x++) {
                render(&cfg, x, y, UINT32_MAX, rgb, NULL);
                render(&cfg, x, y, 0U, other, NULL);
                for (j = 0U; j < 3U; j++) {
                    int delta = (int)rgb[j] - other[j];

                    assert(delta >= -4 && delta <= 4);
                }
            }
        }
    }
    /* Wave/comet propagate horizontally, while ripple expands symmetrically. */
    for (i = 0U; i < 2U; i++) {
        nlk_kb_effect_render_fn_t render = find_effect(i == 0U ? "wave" : "comet");

        render(&cfg, 4U, 0U, 2048U, rgb, NULL);
        render(&cfg, 5U, 4U, 2560U, other, NULL);
        assert(memcmp(rgb, other, sizeof(rgb)) == 0);
    }
    for (i = 0U; i < 14U; i++) {
        find_effect("ripple")(&cfg, i, 0U, 2000U, rgb, NULL);
        find_effect("ripple")(&cfg, 13U - i, 4U, 2000U, other, NULL);
        assert(memcmp(rgb, other, sizeof(rgb)) == 0);
    }
}

int main(void)
{
    struct nlk_kb_light_cfg cfg = {.color_r = 200U, .color_g = 100U, .color_b = 50U};
    uint8_t rgb[3];
    uint8_t other[3];

    assert(nk60_keyboard_profile_init() == 0);
    assert(g_nk60_kb_profile.default_keymap == kb_selected_layout.default_keymap);
    assert(g_nk60_layouts[0].default_keymap == kb_selected_layout.default_keymap);
    test_breath();
    test_rainbow();
    test_additional_effects();
    nk60_effect_static(&cfg, 0U, 0U, 0U, rgb, NULL);
    assert(rgb[0] == 200U && rgb[1] == 100U && rgb[2] == 50U);
    nk60_effect_breath(&cfg, 0U, 0U, 0U, rgb, NULL);
    assert(rgb[0] == 0U && rgb[1] == 0U && rgb[2] == 0U);
    nk60_effect_breath(&cfg, 0U, 0U, 16384U, rgb, NULL);
    assert(rgb[0] == 200U && rgb[1] == 100U && rgb[2] == 50U);
    nk60_effect_rainbow(&cfg, 0U, 0U, 0U, rgb, NULL);
    nk60_effect_rainbow(&cfg, 1U, 0U, 0U, other, NULL);
    assert(rgb[0] != other[0] || rgb[1] != other[1] || rgb[2] != other[2]);
    nk60_effect_rainbow(&cfg, 0U, 0U, 2048U, other, NULL);
    assert(rgb[0] != other[0] || rgb[1] != other[1] || rgb[2] != other[2]);
    assert(g_nk60_light_effects[NK60_EFFECT_BREATH].render == nk60_effect_breath);
    assert(g_nk60_kb_profile.led_count == 61U);
    assert(g_nk60_kb_profile.underglow_leds == 0U);
    assert(g_nk60_kb_profile.light_zone_count == 2U);
    assert(g_nk60_light_zones[1].type == NLK_KB_LIGHT_TYPE_INDICATOR);
    assert(g_nk60_light_zones[1].led_start == 28U);
    assert((g_nk60_kb_profile.kb_flags & NLK_KB_FLAG_LIGHT_UNDERGLOW) == 0U);
    return 0;
}
