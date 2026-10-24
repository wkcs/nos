/* Copyright (C) 2026 NOS contributors. Actual geometry, renderer and ergonomic behavior. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../servers/keyboard/keyboard.h"
#include "../servers/keyboard/lighting.h"
#include "../servers/keyboard/layout.h"
static void geometry(void)
{
    unsigned widths[5] = {0}, count[5] = {0};
    uint64_t leds = 0;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        struct kb_key_position p;
        assert(!kb_layout_position(i, &p));
        unsigned row = p.y / 4;
        assert(row < 5 && p.height == 4 && p.width && p.x == widths[row]);
        assert(p.x + p.width <= KB_LAYOUT_WIDTH && p.led < KB_KEYS);
        assert(!(leds & (UINT64_C(1) << p.led)));
        leds |= UINT64_C(1) << p.led;
        widths[row] += p.width;
        ++count[row];
    }
    for (unsigned r = 0; r < 5; ++r)
        assert(widths[r] == 60);
    assert(count[0] == 14 && count[1] == 14 && count[2] == 13 && count[3] == 12 && count[4] == 8);
    assert(leds == (UINT64_C(1) << KB_KEYS) - 1);
    struct kb_key_position p;
    assert(kb_layout_position(61, &p) == -KB_EINVAL && kb_layout_position(0, NULL) == -KB_EINVAL);
    puts("PASS keyboard_ansi60_geometry_61_unique_per_key_leds");
}
static void effects(void)
{
    struct kb_lighting l = {0};
    struct nk60_rgb out[KB_KEYS], before[KB_KEYS];
    uint8_t wire[KB_LIGHT_SETTINGS_BYTES];
    kb_light_defaults(&l.settings);
    assert(!kb_light_encode(&l.settings, wire));
    struct kb_light_settings saved = l.settings;
    assert(!kb_light_decode(&saved, wire, sizeof(wire)) &&
           !memcmp(&saved, &l.settings, sizeof(saved)));
    wire[223] = 1;
    assert(kb_light_decode(&saved, wire, sizeof(wire)) == -KB_EINVAL);
    assert(!memcmp(&saved, &l.settings, sizeof(saved)));
    for (unsigned mode = 1; mode < KB_LIGHT_MODES; ++mode) {
        for (unsigned i = 0; i < KB_KEYS; ++i)
            l.settings.custom[i] = (struct nk60_rgb){i * 4u, 255 - i * 4u, i};
        l.settings.current_ma = 100;
        kb_light_activity(&l, 1, 250);
        assert(kb_light_render(&l, mode, 255, 7, 2, 1, 0, 300, out));
        assert(l.estimated_ma <= 100);
        for (unsigned i = 0; i < KB_KEYS; ++i)
            assert(out[i].r <= 16 && out[i].g <= 16 && out[i].b <= 16);
    }
    l.settings.current_ma = 300;
    assert(kb_light_render(&l, KB_LIGHT_BREATHE, 255, 1, 0, 1, 0, 7670, out));
    memcpy(before, out, sizeof(out));
    assert(kb_light_render(&l, KB_LIGHT_BREATHE, 255, 1, 0, 1, 0, 7680, out));
    assert(before[0].r == out[0].r && out[0].r); /* no color-wheel modulus discontinuity */
    assert(kb_light_render(&l, KB_LIGHT_WAVE, 255, 1, 0, 1, 0, 1200, out));
    memcpy(before, out, sizeof(out));
    assert(kb_light_render(&l, KB_LIGHT_WAVE, 255, 1, 0, 1, 0, 1600, out));
    assert(memcmp(out, before, sizeof(out)) && memcmp(&out[0], &out[20], sizeof(out[0])));
    memset(l.reactive, 0, sizeof(l.reactive));
    l.pressed = 0;
    l.settings.flags = 0;
    l.last_frame_ms = 2000;
    kb_light_activity(&l, UINT64_C(1) << 29, 2000);
    assert(kb_light_render(&l, KB_LIGHT_REACTIVE, 255, 1, 0, 1, 0, 2000, out));
    assert(out[29].r && !out[30].r);
    assert(kb_light_render(&l, KB_LIGHT_REACTIVE, 255, 1, 0, 1, 0, 2600, out) && !out[29].r);
    l.settings.flags = KB_LIGHT_INDICATORS;
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 0, 1, 2, 1, 0, 2700, out) == 0);
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 2, 1, 1, 2800, out) == 0);
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 2, 0, 0, 2900, out) == 0);
    l.settings.idle_seconds = 1;
    l.last_activity_ms = UINT32_MAX - 500;
    l.pressed = 0;
    assert(!kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 0, 1, 0, 500, out));
    kb_light_activity(&l, 1, 501);
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 0, 1, 0, 502, out));
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 0, 1, 0, 9999,
                           out)); /* held key stays awake */
    puts("PASS keyboard_per_key_effects_current_limit_idle_suspend_codec_and_wrap");
}
static void drain(struct keyboard *k)
{
    while (kb_report_peek(k))
        kb_report_pop(k);
}
static void settle(struct keyboard *k, uint64_t keys, uint32_t *now)
{
    assert(!kb_scan(k, keys, ++*now));
    *now += k->config.debounce_ms;
    assert(!kb_scan(k, keys, *now));
}
static int held(const struct keyboard *k, unsigned u)
{
    return u >= 0xe0 ? !!(k->current.nkro[0] & (1u << (u - 0xe0)))
                     : !!(k->current.nkro[1 + u / 8] & (1u << (u % 8)));
}
static void ergonomic(void)
{
    struct keyboard k;
    struct kb_config config;
    uint32_t now = 0;
    kb_default_config(&config);
    assert(!kb_init(&k, &config));
    settle(&k, (UINT64_C(1) << 41) | 1, &now);
    assert(held(&k, KB_GRAVE) && held(&k, KB_LSHIFT) && !held(&k, KB_ESCAPE));
    drain(&k);
    settle(&k, 1, &now);
    assert(held(&k, KB_GRAVE) && !held(&k, KB_LSHIFT));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, 1, &now);
    assert(held(&k, KB_ESCAPE));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, (UINT64_C(1) << 60) | (UINT64_C(1) << 28), &now);
    assert(k.caps_word && !kb_take_local_events(&k));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, UINT64_C(1) << 29, &now);
    assert(k.caps_word && held(&k, KB_A) && held(&k, KB_LSHIFT));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, UINT64_C(1) << 1, &now);
    assert(k.caps_word && held(&k, KB_1) && !held(&k, KB_LSHIFT));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, UINT64_C(1) << 56, &now);
    assert(!k.caps_word && held(&k, KB_SPACE));
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    settle(&k, (UINT64_C(1) << 60) | (UINT64_C(1) << 28), &now);
    assert(k.caps_word);
    drain(&k);
    settle(&k, 0, &now);
    drain(&k);
    now += 5001;
    assert(!kb_scan(&k, 0, now) && !k.caps_word);
    k.caps_word = 1;
    kb_release_all(&k);
    assert(!k.caps_word);
    puts("PASS keyboard_grave_escape_simultaneous_modifiers_caps_word_and_timeout");
}
void keyboard_features(void)
{
    geometry();
    effects();
    ergonomic();
}
