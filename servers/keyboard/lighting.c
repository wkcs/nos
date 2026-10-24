/* Copyright (C) 2026 NOS contributors. Integer-only effects; LED current is an estimate. */
#include "lighting.h"
#include "layout.h"
#include <string.h>
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void kb_light_defaults(struct kb_light_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->color = (struct nk60_rgb){255, 255, 255};
    for (unsigned i = 0; i < KB_KEYS; ++i)
        s->custom[i] = s->color;
    s->speed = 128;
    s->flags = KB_LIGHT_INDICATORS;
    s->idle_seconds = 300;
    s->current_ma = 300;
}
int kb_light_validate(const struct kb_light_settings *s)
{
    return !s || !s->speed || (s->flags & ~KB_LIGHT_INDICATORS) || s->idle_seconds > 3600 ||
                   s->current_ma < 100 || s->current_ma > 350
               ? -KB_EINVAL
               : 0;
}
int kb_light_encode(const struct kb_light_settings *s, uint8_t out[KB_LIGHT_SETTINGS_BYTES])
{
    if (!out || kb_light_validate(s))
        return -KB_EINVAL;
    memset(out, 0, KB_LIGHT_SETTINGS_BYTES);
    memcpy(out, "KBL1", 4);
    out[4] = 1;
    out[5] = s->speed;
    out[6] = s->flags;
    put16(out + 8, s->idle_seconds);
    put16(out + 10, s->current_ma);
    out[12] = s->color.r;
    out[13] = s->color.g;
    out[14] = s->color.b;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        out[16 + i * 3] = s->custom[i].r;
        out[17 + i * 3] = s->custom[i].g;
        out[18 + i * 3] = s->custom[i].b;
    }
    return 0;
}
int kb_light_decode(struct kb_light_settings *s, const uint8_t *in, size_t size)
{
    struct kb_light_settings candidate;
    if (!s || !in || size != KB_LIGHT_SETTINGS_BYTES || memcmp(in, "KBL1", 4) || in[4] != 1 ||
        in[7] || in[15])
        return -KB_EINVAL;
    for (unsigned i = 199; i < KB_LIGHT_SETTINGS_BYTES; ++i)
        if (in[i])
            return -KB_EINVAL;
    memset(&candidate, 0, sizeof(candidate));
    candidate.speed = in[5];
    candidate.flags = in[6];
    candidate.idle_seconds = get16(in + 8);
    candidate.current_ma = get16(in + 10);
    candidate.color = (struct nk60_rgb){in[12], in[13], in[14]};
    for (unsigned i = 0; i < KB_KEYS; ++i)
        candidate.custom[i] = (struct nk60_rgb){in[16 + i * 3], in[17 + i * 3], in[18 + i * 3]};
    if (kb_light_validate(&candidate))
        return -KB_EINVAL;
    *s = candidate;
    return 0;
}
int kb_light_preferences_encode(const struct kb_lighting *l,
                                uint8_t out[KB_LIGHT_PREFERENCES_BYTES])
{
    if (!l || !out || kb_fx_validate(l->fx.program, KB_FX_PROGRAM_BYTES))
        return -KB_EINVAL;
    int error = kb_light_encode(&l->settings, out);
    if (!error)
        memcpy(out + KB_LIGHT_SETTINGS_BYTES, l->fx.program, KB_FX_PROGRAM_BYTES);
    return error;
}
int kb_light_preferences_decode(struct kb_lighting *l, const uint8_t *in, size_t size)
{
    struct kb_light_settings settings;
    if (!l || !in || (size != KB_LIGHT_SETTINGS_BYTES && size != KB_LIGHT_PREFERENCES_BYTES) ||
        kb_light_decode(&settings, in, KB_LIGHT_SETTINGS_BYTES) ||
        (size == KB_LIGHT_PREFERENCES_BYTES &&
         kb_fx_validate(in + KB_LIGHT_SETTINGS_BYTES, KB_FX_PROGRAM_BYTES)))
        return -KB_EINVAL;
    l->settings = settings;
    if (size == KB_LIGHT_SETTINGS_BYTES)
        kb_fx_defaults(&l->fx);
    else
        kb_fx_set(&l->fx, in + KB_LIGHT_SETTINGS_BYTES, KB_FX_PROGRAM_BYTES);
    return 0;
}
void kb_light_activity(struct kb_lighting *l, uint64_t pressed, uint32_t now)
{
    uint64_t down = pressed & ~l->pressed;
    if (pressed != l->pressed)
        l->last_activity_ms = now;
    for (unsigned i = 0; i < KB_KEYS; ++i)
        if (down & (UINT64_C(1) << i))
            l->reactive[i] = 255;
    l->pressed = pressed;
}
static struct nk60_rgb wheel(unsigned phase)
{
    phase %= 768;
    if (phase < 256)
        return (struct nk60_rgb){255 - phase, phase, 0};
    if (phase < 512)
        return (struct nk60_rgb){0, 511 - phase, phase - 256};
    return (struct nk60_rgb){phase - 512, 0, 767 - phase};
}
int kb_light_render(struct kb_lighting *l, unsigned mode, uint8_t brightness, unsigned layers,
                    unsigned leds, int configured, int suspended, uint32_t now,
                    struct nk60_rgb out[KB_KEYS])
{
    if (!l || !out)
        return 0;
    memset(out, 0, sizeof(*out) * KB_KEYS);
    l->estimated_ma = 0;
    uint32_t delta = now - l->last_frame_ms;
    l->last_frame_ms = now;
    unsigned decay = delta > 500 ? 255 : delta * 255 / 500;
    if (delta && !decay)
        decay = 1;
    for (unsigned i = 0; i < KB_KEYS; ++i)
        l->reactive[i] = l->reactive[i] > decay ? l->reactive[i] - decay : 0;
    if (!configured || suspended || !mode || mode >= KB_LIGHT_MODES || !brightness ||
        kb_light_validate(&l->settings) ||
        (l->settings.idle_seconds && !l->pressed &&
         now - l->last_activity_ms >= (uint32_t)l->settings.idle_seconds * 1000))
        return 0;
    uint64_t ticks = (uint64_t)now * l->settings.speed / 1280u;
    unsigned phase = (unsigned)(ticks % 768u);
    unsigned sum = 0;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        struct nk60_rgb c = l->settings.color;
        unsigned strength = 255;
        if (mode == KB_LIGHT_LAYER)
            c = (struct nk60_rgb){layers & (1u << KB_LAYER_FN) ? 255 : 0,
                                  layers & (1u << KB_LAYER_PN) ? 255 : 0, 255};
        else if (mode == KB_LIGHT_BREATHE) {
            unsigned p = (unsigned)(ticks % 512u);
            strength = p < 256 ? p : 511 - p;
        } else if (mode == KB_LIGHT_RAINBOW)
            c = wheel(phase);
        else if (mode == KB_LIGHT_WAVE) {
            struct kb_key_position p;
            kb_layout_position(i, &p);
            c = wheel(phase + (p.x * 2 + p.width) * 768u / (2 * KB_LAYOUT_WIDTH));
        } else if (mode == KB_LIGHT_REACTIVE)
            strength = l->reactive[i];
        else if (mode == KB_LIGHT_CUSTOM)
            c = l->settings.custom[i];
        if ((l->settings.flags & KB_LIGHT_INDICATORS) && i == 28 && (leds & 2)) {
            c = (struct nk60_rgb){255, 255, 255};
            strength = 255;
        }
        unsigned level = 255;
        out[i].r = (uint8_t)((unsigned)c.r * level * strength / (255u * 255));
        out[i].g = (uint8_t)((unsigned)c.g * level * strength / (255u * 255));
        out[i].b = (uint8_t)((unsigned)c.b * level * strength / (255u * 255));
        sum += out[i].r + out[i].g + out[i].b;
    }
    kb_fx_render(&l->fx, now, out);
    unsigned level = (unsigned)brightness * KB_LIGHT_CHANNEL_LIMIT / 255;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        out[i].r = (uint8_t)((unsigned)out[i].r * level / 255);
        out[i].g = (uint8_t)((unsigned)out[i].g * level / 255);
        out[i].b = (uint8_t)((unsigned)out[i].b * level / 255);
    }
    /* Model: 1mA idle per LED and 20mA at full-scale per RGB channel. */
    unsigned allowance = (l->settings.current_ma - KB_KEYS) * 255u / 20u;
    if (sum > allowance) {
        for (unsigned i = 0; i < KB_KEYS; ++i) {
            out[i].r = (uint8_t)((unsigned)out[i].r * allowance / sum);
            out[i].g = (uint8_t)((unsigned)out[i].g * allowance / sum);
            out[i].b = (uint8_t)((unsigned)out[i].b * allowance / sum);
        }
    }
    sum = 0;
    for (unsigned i = 0; i < KB_KEYS; ++i)
        sum += out[i].r + out[i].g + out[i].b;
    l->estimated_ma = (uint16_t)(KB_KEYS + (sum * 20u + 254) / 255u);
    return 1;
}
