/* Copyright (C) 2026 NOS contributors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../servers/keyboard/keyboard.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
#define BIT(i) ((uint64_t)1 << (i))
static struct keyboard k;
static struct kb_config c;
static uint32_t now;
static unsigned assertions;
static void drain(void) { while (kb_report_peek(&k)) kb_report_pop(&k); }
static void reset(void)
{
    kb_default_config(&c);
    CHECK(!kb_init(&k, &c));
    now = 100;
}
static void scan(uint64_t bits, unsigned ms)
{
    now += ms;
    CHECK(!kb_scan(&k, bits, now));
}
static void settle(uint64_t bits)
{
    scan(bits, 1); scan(bits, c.debounce_ms);
}
static int held(unsigned u)
{
    return u >= 0xe0 ? !!(k.current.nkro[0] & (1u << (u - 0xe0))) :
        !!(k.current.nkro[1 + (u >> 3)] & (1u << (u & 7)));
}
static void debounce(void)
{
    reset();
    scan(BIT(29), 1); scan(0, 2); scan(BIT(29), 2); scan(BIT(29), 4);
    CHECK(!held(KB_A)); scan(BIT(29), 1); CHECK(held(KB_A)); drain();
    scan(0, 1); scan(BIT(29), 2); scan(0, 2); scan(0, 4);
    CHECK(held(KB_A)); scan(0, 1); CHECK(!held(KB_A));
    reset(); now = UINT32_MAX - 3;
    scan(BIT(29), 0); scan(BIT(29), 5); CHECK(held(KB_A));
    CHECK(kb_scan(&k, BIT(29), now - 1) == -KB_EINVAL);
    CHECK(kb_scan(&k, BIT(63), now + 1) == -KB_EINVAL);
    puts("PASS keyboard_symmetric_debounce_wrap");
}
static void layers(void)
{
    reset();
    settle(BIT(60) | BIT(1)); CHECK(held(KB_F1)); CHECK(!held(KB_1)); drain();
    settle(BIT(1)); CHECK(held(KB_F1)); drain();
    settle(0); CHECK(!held(KB_F1)); drain();
    settle(BIT(60) | BIT(15)); CHECK(held(KB_Q)); drain();
    settle(0); drain();
    /* FN overrides PN for shared bindings; physical FN may be released first. */
    settle(BIT(60) | BIT(58) | BIT(1)); CHECK(held(KB_F1)); drain();
    settle(0); drain();
    c.map[0][28] = (struct kb_action){ KB_TOGGLE, 3, 0 };
    c.map[3][29] = (struct kb_action){ KB_KEY, 0, KB_B };
    CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1);
    settle(BIT(28)); drain(); settle(0); drain();
    settle(BIT(29)); CHECK(held(KB_B)); drain(); settle(0); drain();
    settle(BIT(28)); drain(); settle(0); drain();
    settle(BIT(29)); CHECK(held(KB_A));
    puts("PASS keyboard_layer_binding_transparent_toggle");
}
static void rollover(void)
{
    unsigned i;
    reset(); settle(0x7f);
    for (i = 2; i < 8; ++i) CHECK(k.current.boot[i] == 1);
    CHECK(held(KB_ESCAPE) && held(KB_6)); drain();
    settle(BIT(1)); CHECK(k.current.boot[2] == KB_1); drain();
    settle(0); drain();
    c.map[0][30] = c.map[0][29]; CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1);
    settle(BIT(29) | BIT(30)); CHECK(held(KB_A)); drain();
    settle(BIT(30)); CHECK(held(KB_A)); drain();
    settle(0); CHECK(!held(KB_A)); drain();
    settle(BIT(41)); CHECK(held(KB_LSHIFT));
    CHECK(!(k.current.nkro[1 + (KB_LSHIFT >> 3)] & (1u << (KB_LSHIFT & 7))));
    puts("PASS keyboard_rollover_duplicate_modifier");
}
static void tap_hold(void)
{
    const struct kb_report *r;
    reset(); c.map[0][28] = (struct kb_action){ KB_MOD_TAP, 1, KB_ESCAPE };
    CHECK(!kb_init(&k, &c)); settle(BIT(28)); CHECK(!held(KB_ESCAPE) && !held(KB_LCTRL));
    settle(0); r = kb_report_peek(&k); CHECK(r && r->boot[2] == KB_ESCAPE);
    kb_report_pop(&k); r = kb_report_peek(&k); CHECK(r && !r->boot[2]); drain();
    settle(BIT(28)); scan(BIT(28), c.tapping_ms); CHECK(held(KB_LCTRL)); drain();
    settle(0); CHECK(!held(KB_LCTRL)); drain();
    settle(BIT(28) | BIT(29)); CHECK(held(KB_LCTRL) && held(KB_A)); drain(); settle(0); drain();
    c.map[0][30] = (struct kb_action){ KB_MOD_TAP, 2, KB_TAB };
    CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1);
    settle(BIT(28) | BIT(30)); CHECK(held(KB_LCTRL) && held(KB_LSHIFT));
    drain(); settle(0); drain();
    c.map[0][30] = (struct kb_action){ KB_KEY, 0, KB_S };
    c.map[0][28] = (struct kb_action){ KB_LAYER_TAP, 3, KB_ESCAPE };
    c.map[3][29] = (struct kb_action){ KB_KEY, 0, KB_B };
    CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1);
    settle(BIT(28)); settle(BIT(28) | BIT(29)); CHECK(held(KB_B)); drain();
    settle(BIT(29)); CHECK(held(KB_B)); drain(); settle(0); drain();
    c.map[0][28] = (struct kb_action){ KB_ONESHOT, 2, 0 };
    CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1);
    settle(BIT(28)); settle(0); settle(BIT(29));
    CHECK(held(KB_LSHIFT) && held(KB_A)); drain(); settle(0); drain();
    settle(BIT(30)); CHECK(!held(KB_LSHIFT));
    puts("PASS keyboard_tap_hold_oneshot");
}
static void consumer_macro(void)
{
    unsigned phase;
    reset(); c.map[0][28] = (struct kb_action){ KB_CONSUMER, 0, KB_AUDIO_UP };
    c.map[0][29] = (struct kb_action){ KB_CONSUMER, 0, KB_AUDIO_DOWN };
    c.map[0][30] = (struct kb_action){ KB_MACRO, 0, 0 };
    c.macro_length[0] = 2; c.macros[0][0] = KB_A; c.macros[0][1] = KB_B;
    CHECK(!kb_init(&k, &c)); settle(BIT(28) | BIT(29));
    CHECK(k.current.consumer[0] == KB_AUDIO_UP); drain();
    settle(BIT(29)); CHECK(k.current.consumer[0] == KB_AUDIO_DOWN); drain(); settle(0); drain();
    settle(BIT(30)); CHECK(held(KB_A));
    scan(BIT(30), 100); CHECK(held(KB_A)); /* pending report applies backpressure */
    for (phase = 0; phase < 4; ++phase) { drain(); scan(BIT(30), c.macro_ms); }
    CHECK(!held(KB_A) && !held(KB_B) && !k.macro_running); drain(); settle(0); drain();
    c.map[0][28] = (struct kb_action){ KB_LOCAL, KB_NKRO_TOGGLE, 0 };
    CHECK(!kb_reconfigure(&k, &c)); drain(); scan(0, 1); settle(BIT(28));
    CHECK(k.config.nkro == 0 && kb_take_local_events(&k) == (1u << KB_NKRO_TOGGLE));
    CHECK(!kb_take_local_events(&k));
    puts("PASS keyboard_consumer_macro_backpressure_local");
}
static void failsafe(void)
{
    unsigned i;
    struct kb_config invalid;
    reset();
    for (i = 0; i < KB_REPORT_QUEUE; ++i) settle(i & 1 ? 0 : BIT(29));
    CHECK(k.count == KB_REPORT_QUEUE);
    scan(BIT(29), 1); now += c.debounce_ms;
    CHECK(kb_scan(&k, BIT(29), now) == -KB_ENOSPC);
    CHECK(k.blocked && k.overflows == 1 && k.count == 1 && !held(KB_A));
    drain(); scan(BIT(29), 100); CHECK(!held(KB_A)); scan(0, 1);
    settle(BIT(29)); CHECK(held(KB_A)); drain();
    invalid = c; invalid.map[0][29].type = 255;
    CHECK(kb_reconfigure(&k, &invalid) == -KB_EINVAL && held(KB_A));
    c.map[0][29].code = KB_B; CHECK(!kb_reconfigure(&k, &c)); CHECK(!held(KB_A)); drain();
    scan(BIT(29), 100); CHECK(!held(KB_B)); scan(0, 1); settle(BIT(29)); CHECK(held(KB_B));
    kb_release_all(&k); CHECK(!held(KB_B));
    puts("PASS keyboard_queue_fail_closed_atomic_remap");
}

struct flash { uint8_t bytes[8192]; int fail_at, calls, cut, remaining; unsigned erases; };
static int fault(struct flash *f) { return ++f->calls == f->fail_at; }
static int read_flash(void *context, uint32_t offset, void *data, size_t bytes)
{
    struct flash *f = context;
    if (fault(f) || offset > sizeof(f->bytes) || bytes > sizeof(f->bytes) - offset) return -1;
    memcpy(data, f->bytes + offset, bytes); return 0;
}
static int erase_flash(void *context, uint32_t offset, size_t bytes)
{
    struct flash *f = context;
    if (fault(f) || offset % 4096 || bytes != 4096 || offset > 4096) return -1;
    memset(f->bytes + offset, 255, bytes); ++f->erases; return 0;
}
static int write_flash(void *context, uint32_t offset, const void *data, size_t bytes)
{
    struct flash *f = context; const uint8_t *p = data; size_t i;
    if (fault(f) || offset > sizeof(f->bytes) || bytes > sizeof(f->bytes) - offset) return -1;
    for (i = 0; i < bytes; ++i) {
        if (f->cut && !f->remaining--) return -1;
        CHECK((f->bytes[offset + i] & p[i]) == p[i]); f->bytes[offset + i] &= p[i];
    }
    return 0;
}
static void set32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void reseal(uint8_t *record)
{
    set32(record + 12, 0); set32(record + 20, 0);
    set32(record + 12, kb_crc32(record + 4, KB_CONFIG_RECORD_BYTES - 4));
    set32(record + 20, 0x54494d43u);
}
static void persistence(void)
{
    struct flash f, before;
    struct kb_store store = { &f, read_flash, erase_flash, write_flash };
    struct kb_config loaded, unchanged;
    uint8_t wire[KB_CONFIG_WIRE_BYTES];
    uint32_t generation, saved;
    int calls, point;
    reset(); memset(&f, 0, sizeof(f)); memset(f.bytes, 255, sizeof(f.bytes));
    generation = 99; loaded = c; loaded.brightness = 77; unchanged = loaded;
    CHECK(kb_config_load(&store, 0, &loaded, &generation) == -KB_EIO);
    CHECK(!memcmp(&loaded, &unchanged, sizeof(loaded)) && generation == 99);
    CHECK(kb_crc32("123456789", 9) == 0xcbf43926u);
    CHECK(!kb_config_encode(&c, wire, sizeof(wire)));
    CHECK(!kb_config_decode(&loaded, wire, sizeof(wire)) && !memcmp(&c, &loaded, sizeof(c)));
    wire[12] = 255; unchanged = loaded;
    CHECK(kb_config_decode(&loaded, wire, sizeof(wire)) == -KB_EINVAL);
    CHECK(!memcmp(&loaded, &unchanged, sizeof(loaded)));
    CHECK(!kb_config_save(&store, 0, &c, &generation) && generation == 1);
    before = f; c.brightness = 200; f.calls = 0;
    CHECK(!kb_config_save(&store, 0, &c, &generation) && generation == 2); calls = f.calls;
    CHECK(!kb_config_load(&store, 0, &loaded, &saved) && saved == 2 && loaded.brightness == 200);
    /* Every callback failure leaves the old committed generation loadable.
     * Failure after commit can legitimately leave the new generation too. */
    for (point = 1; point <= calls; ++point) {
        f = before; f.calls = 0; f.fail_at = point; generation = 55;
        CHECK(kb_config_save(&store, 0, &c, &generation) == -KB_EIO);
        CHECK(generation == 55); f.fail_at = 0;
        CHECK(!kb_config_load(&store, 0, &loaded, &saved));
        CHECK((saved == 1 && loaded.brightness == 128) || (saved == 2 && loaded.brightness == 200));
    }
    for (point = 0; point < 1080; ++point) {
        f = before; f.calls = 0; f.cut = 1; f.remaining = point; generation = 55;
        CHECK(kb_config_save(&store, 0, &c, &generation) == -KB_EIO && generation == 55);
        f.cut = 0;
        CHECK(!kb_config_load(&store, 0, &loaded, &saved) && saved == 1 && loaded.brightness == 128);
    }
    f = before; f.calls = 0; CHECK(!kb_config_save(&store, 0, &c, &generation));
    f.bytes[4096 + 42] ^= 1;
    CHECK(!kb_config_load(&store, 0, &loaded, &saved) && saved == 1 && loaded.brightness == 128);
    /* A checksum-valid but invalid action schema must fall back too. */
    f = before; f.calls = 0; CHECK(!kb_config_save(&store, 0, &c, &generation));
    f.bytes[4096 + 24 + 12] = 255; reseal(f.bytes + 4096);
    CHECK(!kb_config_load(&store, 0, &loaded, &saved) && saved == 1);
    /* Serial generation comparison remains correct across UINT32_MAX. */
    f = before; f.calls = 0; set32(f.bytes + 8, UINT32_MAX - 1); reseal(f.bytes);
    CHECK(!kb_config_save(&store, 0, &c, &generation) && generation == UINT32_MAX);
    CHECK(!kb_config_save(&store, 0, &c, &generation) && generation == 0);
    CHECK(!kb_config_load(&store, 0, &loaded, &saved) && saved == 0 && loaded.brightness == 200);
    CHECK(kb_config_save(&store, 1, &c, &generation) == -KB_EINVAL);
    CHECK(kb_config_load(&store, UINT32_MAX - 4095, &loaded, &saved) == -KB_EINVAL);
    puts("PASS keyboard_config_crc_dual_slot_fault_atomicity");
}
static void randomized(void)
{
    uint32_t seed = 0x1badb002;
    uint64_t bits = 0;
    unsigned i;
    reset();
    for (i = 0; i < 100000; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        bits ^= BIT(seed % KB_KEYS);
        scan(bits, seed % 9);
        drain(); ++assertions;
    }
    settle(0); drain();
    CHECK(!k.current.boot[0] && !k.current.boot[2] && !k.current.consumer[0]);
    puts("PASS keyboard_stateful_fuzz_100000");
}
int main(void)
{
    debounce(); layers(); rollover(); tap_hold(); consumer_macro(); failsafe(); persistence(); randomized();
    printf("PASS keyboard_all sizeof_keyboard=%zu sizeof_config=%zu iterations=%u\n", sizeof(k), sizeof(c), assertions);
    return 0;
}
