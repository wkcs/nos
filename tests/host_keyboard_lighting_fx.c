/* Copyright (C) 2026 NOS contributors. Real input events, compositor and lifetime tests. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../servers/keyboard/lighting.h"
static struct keyboard k;
static struct kb_lighting l;
static uint8_t program[KB_FX_PROGRAM_BYTES];
static unsigned counts[KB_EVENT_RESET + 1];
static struct kb_event latest[KB_EVENT_RESET + 1];
static uint32_t now;
static void put16(uint8_t *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void mask(uint8_t *r, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        r[20 + i] = (uint8_t)(v >> (i * 8));
}
static void hook(void *unused, const struct kb_event *e)
{
    (void)unused;
    assert(e->type <= KB_EVENT_RESET);
    ++counts[e->type];
    latest[e->type] = *e;
    kb_fx_keyboard_event(&l.fx, e);
}
static void start(unsigned rules)
{
    struct kb_config config;
    kb_default_config(&config);
    assert(!kb_init(&k, &config));
    memset(&l, 0, sizeof(l));
    kb_light_defaults(&l.settings);
    kb_fx_defaults(&l.fx);
    memset(counts, 0, sizeof(counts));
    memset(latest, 0, sizeof(latest));
    kb_set_event_handler(&k, hook, NULL);
    now = 100;
    kb_fx_observe(&l.fx, &k, 1, 0, 1, now, now);
    memset(program, 0, sizeof(program));
    memcpy(program, "KLFX", 4);
    program[4] = 1;
    program[5] = rules;
    put16(program + 6, KB_FX_ENABLED | KB_FX_DARK_BASE);
    put16(program + 8, 40);
}
static uint8_t *rule(unsigned index, unsigned trigger, unsigned match, unsigned value,
                     unsigned target, unsigned effect, unsigned blend, unsigned duration,
                     unsigned threshold, unsigned red, unsigned green, unsigned blue)
{
    uint8_t *r = program + 16 + index * 32;
    r[0] = trigger;
    r[1] = match;
    put16(r + 2, value);
    r[4] = target;
    r[5] = effect;
    r[6] = blend;
    r[7] = index;
    r[8] = r[11] = red;
    r[9] = r[12] = green;
    r[10] = r[13] = blue;
    put16(r + 14, duration);
    put16(r + 16, 500);
    put16(r + 18, threshold);
    r[29] = 255;
    if (target == KB_FX_MASK)
        mask(r, UINT64_C(1) << 30);
    return r;
}
static void install(void)
{
    assert(!kb_fx_set(&l.fx, program, sizeof(program)));
}
static void drain(struct keyboard *q)
{
    while (kb_report_peek(q))
        kb_report_pop(q);
}
static void scan(uint64_t keys, unsigned delta)
{
    now += delta;
    assert(!kb_scan(&k, keys, now));
    kb_fx_observe(&l.fx, &k, kb_active_layers(&k), l.fx.leds, 1, l.last_activity_ms, now);
    drain(&k);
}
static void settle(uint64_t keys)
{
    scan(keys, 1);
    scan(keys, k.config.debounce_ms);
}
static void render(struct nk60_rgb *out)
{
    memset(out, 0, sizeof(*out) * KB_KEYS);
    kb_fx_render(&l.fx, now, out);
}
static void physical_and_gestures(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(4);
    rule(0, KB_FX_PRESS, KB_FX_KEY, 29, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 500, 0, 255, 0, 0);
    uint8_t *r = rule(1, KB_FX_RELEASE, KB_FX_KEY, 29, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 500,
                      0, 0, 255, 0);
    mask(r, UINT64_C(1) << 30);
    r = rule(2, KB_FX_TAP, KB_FX_USAGE, KB_A, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 500, 200, 0,
             0, 255);
    mask(r, UINT64_C(1) << 31);
    r = rule(3, KB_FX_DOUBLE_TAP, KB_FX_KEY, 29, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 500, 250,
             255, 255, 0);
    mask(r, UINT64_C(1) << 32);
    install();
    settle(UINT64_C(1) << 29);
    render(out);
    assert(out[29].r == 255 && !out[30].g);
    settle(0);
    render(out);
    assert(out[30].g == 255 && out[31].b == 255 && !out[32].r);
    settle(UINT64_C(1) << 29);
    settle(0);
    render(out);
    assert(out[32].r == 255 && out[32].g == 255);
    assert(counts[KB_EVENT_PRESS] == 2 && counts[KB_EVENT_RELEASE] == 2 &&
           latest[KB_EVENT_RELEASE].code == KB_A);
    scan(0, 1000);
    render(out);
    for (unsigned i = 0; i < 61; ++i)
        assert(!out[i].r && !out[i].g && !out[i].b);
    puts("PASS lighting_fx_actual_press_release_usage_tap_doubletap_overlap_and_expiry");
}
static void hold_decisions(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(3);
    k.config.map[0][28] = (struct kb_action){KB_MOD_TAP, 1, KB_ESCAPE};
    rule(0, KB_FX_TAP, KB_FX_ACTION, KB_MOD_TAP | 0xff00, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE,
         500, 200, 0, 0, 255);
    rule(1, KB_FX_HOLD_DECIDED, KB_FX_ACTION, KB_MOD_TAP | 0xff00, KB_FX_SELF, KB_FX_SOLID,
         KB_FX_REPLACE, 500, 0, 255, 0, 0);
    rule(2, KB_FX_HELD, KB_FX_KEY, 29, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 0, 100, 0, 255, 0);
    install();
    settle(UINT64_C(1) << 28);
    settle(0);
    render(out);
    assert(out[28].b && !counts[KB_EVENT_HOLD]);
    kb_fx_clear(&l.fx);
    settle(UINT64_C(1) << 28);
    scan(UINT64_C(1) << 28, 200);
    assert(counts[KB_EVENT_HOLD] == 1);
    render(out);
    assert(out[28].r && !out[28].b);
    unsigned taps = counts[KB_EVENT_TAP];
    settle(0);
    assert(counts[KB_EVENT_TAP] == taps);
    kb_fx_clear(&l.fx);
    settle(UINT64_C(1) << 28);
    settle((UINT64_C(1) << 28) | (UINT64_C(1) << 29));
    assert(counts[KB_EVENT_HOLD] == 2);
    render(out);
    assert(!out[29].g);
    scan((UINT64_C(1) << 28) | (UINT64_C(1) << 29), 101);
    render(out);
    assert(out[29].g);
    settle(0);
    render(out);
    assert(!out[29].g);
    puts("PASS lighting_fx_modtap_tap_hold_interrupt_decision_and_configurable_held_threshold");
}
static void contexts_and_macro(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(5);
    uint8_t *r = rule(0, KB_FX_LAYER_ACTIVE, KB_FX_LAYER, 2, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE,
                      0, 0, 255, 0, 0);
    mask(r, 1);
    r = rule(1, KB_FX_LED_ACTIVE, KB_FX_HOST_LED, 1, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 0, 0,
             0, 255, 0);
    mask(r, UINT64_C(1) << 28);
    r = rule(2, KB_FX_MODS_ACTIVE, KB_FX_MODIFIERS, 2, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 0, 0,
             0, 0, 255);
    mask(r, UINT64_C(1) << 41);
    r = rule(3, KB_FX_CAPS_ACTIVE, KB_FX_ANY, 0, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 0, 0, 255,
             255, 0);
    mask(r, UINT64_C(1) << 27);
    rule(4, KB_FX_MACRO_ACTIVE, KB_FX_MACRO, 0, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 0, 0, 255,
         0, 255);
    install();
    settle(UINT64_C(1) << 60);
    render(out);
    assert(out[0].r);
    settle(0);
    render(out);
    assert(!out[0].r);
    kb_fx_observe(&l.fx, &k, 1, 2, 1, now, now);
    render(out);
    assert(out[28].g);
    kb_fx_observe(&l.fx, &k, 1, 0, 1, now, now);
    render(out);
    assert(!out[28].g);
    settle(UINT64_C(1) << 41);
    render(out);
    assert(out[41].b);
    settle(0);
    render(out);
    assert(!out[41].b);
    k.caps_word = 1;
    kb_fx_observe(&l.fx, &k, 1, 0, 1, now, now);
    render(out);
    assert(out[27].r && out[27].g);
    k.caps_word = 0;
    k.config.map[0][30] = (struct kb_action){KB_MACRO, 0, 0};
    k.config.macro_length[0] = 1;
    k.config.macros[0][0] = KB_A;
    settle(UINT64_C(1) << 30);
    render(out);
    assert(out[30].r && out[30].b && counts[KB_EVENT_MACRO_START] == 1);
    for (unsigned i = 0; i < 4; ++i)
        scan(UINT64_C(1) << 30, 20);
    assert(!k.macro_running && counts[KB_EVENT_MACRO_END] == 1);
    render(out);
    assert(!out[30].r);
    kb_release_all(&k);
    assert(counts[KB_EVENT_RESET]);
    puts("PASS lighting_fx_layers_host_leds_modifiers_caps_word_macro_lifetime_and_reset");
}
static void macro_busy_timing(void)
{
    start(1);
    rule(0, KB_FX_MACRO_ACTIVE, KB_FX_MACRO, 0, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 0, 0, 255,
         0, 0);
    k.config.map[0][30] = (struct kb_action){KB_MACRO, 0, 0};
    k.config.macro_length[0] = 2;
    k.config.macros[0][0] = KB_A;
    k.config.macros[0][1] = KB_B;
    k.config.macro_ms = 1000;
    install();
    settle(UINT64_C(1) << 30);
    uint32_t started = k.macro_started_ms;
    settle(0);
    now += 1;
    assert(!kb_scan(&k, UINT64_C(1) << 30, now));
    now += k.config.debounce_ms;
    assert(kb_scan(&k, UINT64_C(1) << 30, now) == -KB_EBUSY);
    assert(k.macro_started_ms == started && counts[KB_EVENT_MACRO_START] == 1);
    drain(&k);
    for (unsigned i = 0; i < 5; ++i)
        scan(UINT64_C(1) << 30, 1000);
    assert(counts[KB_EVENT_MACRO_END] == 1 && latest[KB_EVENT_MACRO_END].duration_ms >= 4000);
    puts("PASS lighting_fx_macro_restart_rejection_preserves_real_start_and_end_time");
}
static void blend_and_bounds(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(4);
    rule(0, KB_FX_ALWAYS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_SOLID, KB_FX_REPLACE, 0, 0, 100, 0, 0);
    rule(1, KB_FX_ALWAYS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_SOLID, KB_FX_ADD, 0, 0, 0, 0, 100);
    rule(2, KB_FX_ALWAYS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_SOLID, KB_FX_MAX, 0, 0, 0, 100, 0);
    rule(3, KB_FX_ALWAYS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_SOLID, KB_FX_MULTIPLY, 0, 0, 128, 128,
         128);
    install();
    render(out);
    for (unsigned i = 0; i < 61; ++i)
        assert(out[i].r == 50 && out[i].g == 50 && out[i].b == 50);
    uint8_t before[KB_FX_PROGRAM_BYTES];
    memcpy(before, l.fx.program, sizeof(before));
    program[16 + 3 * 32 + 16] = 0;
    program[16 + 3 * 32 + 17] = 0;
    assert(kb_fx_set(&l.fx, program, sizeof(program)) == -KB_EINVAL);
    assert(!memcmp(before, l.fx.program, sizeof(before)));
    start(1);
    rule(0, KB_FX_PRESS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_RIPPLE, KB_FX_ADD, 500, 0, 255, 0, 0);
    install();
    settle(UINT64_C(1) << 29);
    render(out);
    assert(out[29].r && !out[0].r);
    for (unsigned i = 0; i < 20; ++i) {
        struct kb_event e = {.now_ms = now + i,
                             .key = (uint8_t)i,
                             .type = KB_EVENT_PRESS,
                             .action = KB_KEY,
                             .code = KB_A,
                             .layers = 1};
        kb_fx_keyboard_event(&l.fx, &e);
    }
    assert(l.fx.dropped == 5);
    unsigned active = 0;
    for (unsigned i = 0; i < KB_FX_INSTANCES; ++i)
        active += l.fx.instances[i].active;
    assert(active == KB_FX_INSTANCES);
    l.fx.dropped = UINT32_MAX;
    struct kb_event e = {.now_ms = now + 100, .key = 1, .type = KB_EVENT_PRESS};
    kb_fx_keyboard_event(&l.fx, &e);
    assert(l.fx.dropped == UINT32_MAX);
    l.settings.current_ma = 100;
    assert(kb_light_render(&l, KB_LIGHT_STATIC, 255, 1, 2, 1, 0, now, out) &&
           l.estimated_ma <= 100);
    assert(!kb_light_render(&l, KB_LIGHT_OFF, 255, 1, 2, 1, 0, now, out));
    assert(!kb_light_render(&l, KB_LIGHT_STATIC, 0, 1, 2, 1, 0, now, out));
    kb_fx_observe(&l.fx, &k, 1, 0, 0, now, now);
    for (unsigned i = 0; i < KB_FX_INSTANCES; ++i)
        assert(!l.fx.instances[i].active);
    puts("PASS "
         "lighting_fx_priority_blends_ripple_bounded_instances_saturation_and_global_power_limits");
}
static void temporal_context_and_wire(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(3);
    put16(program + 10, 100);
    uint8_t *r = rule(0, KB_FX_IDLE, KB_FX_ANY, 0, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 500, 0,
                      255, 0, 0);
    mask(r, 1);
    r = rule(1, KB_FX_WAKE, KB_FX_ANY, 0, KB_FX_MASK, KB_FX_SOLID, KB_FX_REPLACE, 500, 0, 0, 255,
             0);
    mask(r, 2);
    r = rule(2, KB_FX_PRESS, KB_FX_MODIFIERS, 2, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 500, 0, 0,
             0, 255);
    install();
    now += 101;
    kb_fx_observe(&l.fx, &k, 1, 0, 1, 100, now);
    render(out);
    assert(out[0].r && !out[1].g);
    kb_fx_observe(&l.fx, &k, 1, 0, 1, now, now);
    render(out);
    assert(out[1].g);
    settle((UINT64_C(1) << 41) | (UINT64_C(1) << 29));
    render(out);
    assert(out[29].b); /* same scan modifier */
    uint8_t preferences[KB_LIGHT_PREFERENCES_BYTES];
    assert(!kb_light_preferences_encode(&l, preferences));
    struct kb_lighting loaded = {0};
    kb_fx_defaults(&loaded.fx);
    assert(!kb_light_preferences_decode(&loaded, preferences, sizeof(preferences)) &&
           !memcmp(loaded.fx.program, l.fx.program, KB_FX_PROGRAM_BYTES));
    preferences[KB_LIGHT_SETTINGS_BYTES + 12] = 1;
    assert(kb_light_preferences_decode(&loaded, preferences, sizeof(preferences)) == -KB_EINVAL);
    assert(!kb_light_preferences_decode(&loaded, preferences, KB_LIGHT_SETTINGS_BYTES) &&
           !loaded.fx.program[5]);
    /* End-to-end timing is unsigned and remains correct at millisecond wrap. */
    start(1);
    rule(0, KB_FX_PRESS, KB_FX_ANY, 0, KB_FX_SELF, KB_FX_FADE, KB_FX_REPLACE, 500, 0, 255, 0, 0);
    install();
    now = UINT32_MAX - 20;
    k.clock_valid = 0;
    settle(UINT64_C(1) << 29);
    scan(UINT64_C(1) << 29, 50);
    render(out);
    assert(out[29].r);
    scan(UINT64_C(1) << 29, 600);
    render(out);
    assert(!out[29].r);
    puts(
        "PASS "
        "lighting_fx_idle_wake_simultaneous_modifier_binding_preferences_migration_and_clock_wrap");
}
static void conditions_and_sources(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(1);
    uint8_t *r = rule(0, KB_FX_PRESS, KB_FX_KEY, 29, KB_FX_SELF, KB_FX_SOLID, KB_FX_REPLACE, 500, 0,
                      0, 0, 255);
    r[28] = KB_FX_ANY_MODS;
    r[30] = 4u | (8u << 4);
    r[31] = 0x22;
    install();
    settle((UINT64_C(1) << 60) | (UINT64_C(1) << 41) | (UINT64_C(1) << 29));
    render(out);
    assert(out[29].b);
    settle(0);
    kb_fx_clear(&l.fx);
    settle(UINT64_C(1) << 29);
    render(out);
    assert(!out[29].b);
    settle(0);
    kb_fx_clear(&l.fx);
    settle((UINT64_C(1) << 60) | (UINT64_C(1) << 52) | (UINT64_C(1) << 29));
    render(out);
    assert(out[29].b);
    settle(0);
    kb_fx_clear(&l.fx);
    r[28] = KB_FX_NO_MODS;
    install();
    settle((UINT64_C(1) << 60) | (UINT64_C(1) << 41) | (UINT64_C(1) << 29));
    render(out);
    assert(!out[29].b);
    settle(0);
    kb_fx_clear(&l.fx);
    settle((UINT64_C(1) << 60) | (UINT64_C(1) << 29));
    render(out);
    assert(out[29].b);
    settle(0);
    kb_fx_clear(&l.fx);
    r[28] = 0;
    r[31] = 3;
    install();
    settle((UINT64_C(1) << 60) | (UINT64_C(1) << 53) | (UINT64_C(1) << 41) | (UINT64_C(1) << 29));
    render(out);
    assert(out[29].b);
    start(1);
    rule(0, KB_FX_PRESS, KB_FX_ACTION, KB_GRAVE_ESCAPE | 0xff00, KB_FX_SELF, KB_FX_SOLID,
         KB_FX_REPLACE, 500, 0, 0, 255, 0);
    install();
    settle(1 | (UINT64_C(1) << 41));
    render(out);
    assert(k.key[0].origin_type == KB_GRAVE_ESCAPE && k.key[0].action.code == KB_GRAVE &&
           out[0].g && !out[41].g);
    start(1);
    rule(0, KB_FX_HELD, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_RIPPLE, KB_FX_REPLACE, 0, 20, 255, 0, 0);
    install();
    settle(UINT64_C(1) << 29);
    scan(UINT64_C(1) << 29, 100);
    settle((UINT64_C(1) << 29) | (UINT64_C(1) << 30));
    scan((UINT64_C(1) << 29) | (UINT64_C(1) << 30), 20);
    render(out);
    assert(out[30].r > out[29].r);
    puts("PASS "
         "lighting_fx_required_excluded_layers_any_all_no_modifiers_origin_actions_and_bounded_"
         "wide_held_sources");
}
static void instance_composition_order(void)
{
    struct nk60_rgb out[KB_KEYS];
    start(1);
    rule(0, KB_FX_PRESS, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_RAINBOW, KB_FX_REPLACE, 1000, 0, 255, 0, 0);
    install();
    for (unsigned i = 0; i < 17; ++i) {
        struct kb_event e = {
            .type = KB_EVENT_PRESS, .key = (uint8_t)i, .now_ms = 100 + i * 20, .layers = 1};
        kb_fx_keyboard_event(&l.fx, &e);
    }
    now = 420;
    render(out);
    assert(out[0].r == 255 && !out[0].g && !out[0].b && l.fx.dropped == 1);
    unsigned dropped = l.fx.dropped;
    struct kb_event e = {.type = KB_EVENT_PRESS, .key = 1, .now_ms = 2000, .layers = 1};
    kb_fx_keyboard_event(&l.fx, &e);
    assert(l.fx.dropped ==
           dropped); /* expired instances are reclaimed before accounting eviction */
    unsigned active = 0;
    for (unsigned i = 0; i < KB_FX_INSTANCES; ++i)
        active += l.fx.instances[i].active;
    assert(active == 1);
    start(1);
    uint8_t *r = rule(0, KB_FX_PRESS, KB_FX_ANY, 0, KB_FX_SELF, KB_FX_FADE, KB_FX_REPLACE, 500, 0,
                      255, 0, 0);
    r[28] = KB_FX_REVERSE;
    install();
    settle(UINT64_C(1) << 29);
    render(out);
    assert(!out[29].r);
    scan(UINT64_C(1) << 29, 250);
    render(out);
    assert(out[29].r > 100 && out[29].r < 150);
    puts("PASS lighting_fx_newest_overlay_order_and_expired_instance_reclamation");
}
static void deterministic_fuzz(void)
{
    start(2);
    rule(0, KB_FX_PRESS, KB_FX_ANY, 0, KB_FX_SELF, KB_FX_FADE, KB_FX_ADD, 500, 0, 255, 0, 0);
    rule(1, KB_FX_RELEASE, KB_FX_ANY, 0, KB_FX_ALL, KB_FX_WAVE, KB_FX_MAX, 500, 0, 0, 0, 255);
    install();
    struct keyboard plain;
    assert(!kb_init(&plain, &k.config));
    uint32_t seed = 0x12bc987a;
    uint64_t keys = 0;
    for (unsigned i = 0; i < 50000; ++i) {
        seed = seed * 1664525 + 1013904223;
        keys ^= UINT64_C(1) << (seed % 61);
        now += 1 + (seed >> 24) % 20;
        int a = kb_scan(&k, keys, now), b = kb_scan(&plain, keys, now);
        assert(a == b);
        assert(!memcmp(k.current.boot, plain.current.boot, 8) &&
               !memcmp(k.current.nkro, plain.current.nkro, 33) &&
               !memcmp(k.current.consumer, plain.current.consumer, 2) &&
               k.blocked == plain.blocked);
        kb_fx_observe(&l.fx, &k, kb_active_layers(&k), 0, 1, now, now);
        if (!(i % 47)) {
            struct nk60_rgb out[61];
            render(out);
        }
        drain(&k);
        drain(&plain);
    }
    uint8_t original[KB_FX_PROGRAM_BYTES], bad[KB_FX_PROGRAM_BYTES];
    memcpy(original, l.fx.program, sizeof(original));
    for (unsigned i = 0; i < 10000; ++i) {
        memcpy(bad, original, sizeof(bad));
        seed = seed * 1664525 + 1013904223;
        bad[seed % sizeof(bad)] ^= (uint8_t)(1 + (seed >> 24));
        int result = kb_fx_set(&l.fx, bad, sizeof(bad));
        if (result)
            assert(!memcmp(original, l.fx.program, sizeof(original)));
        else {
            struct nk60_rgb out[61];
            render(out);
            assert(!kb_fx_set(&l.fx, original, sizeof(original)));
        }
    }
    puts("PASS lighting_fx_50000_input_equivalence_and_10000_hostile_programs");
}
void keyboard_lighting_fx(void)
{
    physical_and_gestures();
    hold_decisions();
    contexts_and_macro();
    macro_busy_timing();
    blend_and_bounds();
    temporal_context_and_wire();
    conditions_and_sources();
    instance_composition_order();
    deterministic_fuzz();
}
