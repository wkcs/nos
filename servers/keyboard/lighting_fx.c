/* Copyright (C) 2026 NOS contributors. Fixed-work, integer-only effect rules. */
#include "lighting_fx.h"
#include "layout.h"
#include <string.h>
static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint64_t u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static int enabled(const struct kb_fx *f)
{
    return (u16(f->program + 6) & KB_FX_ENABLED) && f->program[5];
}
static int state_trigger(unsigned t)
{
    return t == KB_FX_ALWAYS || t == KB_FX_HELD || t == KB_FX_LAYER_ACTIVE ||
           t == KB_FX_LED_ACTIVE || t == KB_FX_CAPS_ACTIVE || t == KB_FX_MACRO_ACTIVE ||
           t == KB_FX_MODS_ACTIVE;
}
static int key_trigger(unsigned t)
{
    return t == KB_FX_PRESS || t == KB_FX_RELEASE || t == KB_FX_TAP || t == KB_FX_DOUBLE_TAP ||
           t == KB_FX_HELD || t == KB_FX_HOLD_DECIDED || t == KB_FX_MACRO_START ||
           t == KB_FX_MACRO_END || t == KB_FX_MACRO_ACTIVE;
}
void kb_fx_defaults(struct kb_fx *f)
{
    memset(f, 0, sizeof(*f));
    memcpy(f->program, "KLFX", 4);
    f->program[4] = 1;
    f->program[8] = 100;
}
int kb_fx_validate(const uint8_t *p, size_t size)
{
    if (!p || size != KB_FX_PROGRAM_BYTES || memcmp(p, "KLFX", 4) || p[4] != 1 ||
        p[5] > KB_FX_RULES || (u16(p + 6) & ~(KB_FX_ENABLED | KB_FX_DARK_BASE)) ||
        u16(p + 8) < 40 || u16(p + 8) > 1000 || u16(p + 10) > 60000 || p[12] || p[13] || p[14] ||
        p[15])
        return -KB_EINVAL;
    unsigned priority = 0;
    for (unsigned i = 0; i < KB_FX_RULES; ++i) {
        const uint8_t *r = p + 16 + i * 32;
        if (i >= p[5]) {
            for (unsigned j = 0; j < 32; ++j)
                if (r[j])
                    return -KB_EINVAL;
            continue;
        }
        unsigned t = r[0], m = r[1], v = u16(r + 2), duration = u16(r + 14), period = u16(r + 16),
                 threshold = u16(r + 18);
        if (t >= KB_FX_TRIGGER_COUNT || m >= KB_FX_MATCH_COUNT || r[4] > KB_FX_ALL ||
            r[5] >= KB_FX_EFFECT_COUNT || r[6] > KB_FX_MULTIPLY || r[7] < priority ||
            duration > 60000 || (!state_trigger(t) && !duration) || period < 20 || period > 60000 ||
            (r[28] &
             ~(KB_FX_SINGLE | KB_FX_REVERSE | KB_FX_VERTICAL | KB_FX_ANY_MODS | KB_FX_NO_MODS)) ||
            ((r[30] & 15u) & (r[30] >> 4)) ||
            ((r[28] & (KB_FX_ANY_MODS | KB_FX_NO_MODS)) == (KB_FX_ANY_MODS | KB_FX_NO_MODS)) ||
            ((r[28] & (KB_FX_ANY_MODS | KB_FX_NO_MODS)) && !r[31]) ||
            ((r[28] & KB_FX_VERTICAL) && r[5] != KB_FX_WAVE) ||
            ((r[28] & KB_FX_REVERSE) && r[5] == KB_FX_SOLID) ||
            ((r[28] & KB_FX_SINGLE) && state_trigger(t)))
            return -KB_EINVAL;
        priority = r[7];
        if ((r[4] == KB_FX_SELF || r[5] == KB_FX_RIPPLE) && !key_trigger(t))
            return -KB_EINVAL;
        if (r[4] == KB_FX_MASK ? (!u64(r + 20) || (u64(r + 20) >> KB_KEYS)) : u64(r + 20) != 0)
            return -KB_EINVAL;
        if ((m == KB_FX_KEY || m == KB_FX_USAGE || m == KB_FX_ACTION || m == KB_FX_CONSUMER) &&
            !key_trigger(t))
            return -KB_EINVAL;
        if ((m == KB_FX_ANY && v) || (m == KB_FX_KEY && v >= KB_KEYS) ||
            (m == KB_FX_USAGE && (v < 4 || v > KB_RGUI)) ||
            (m == KB_FX_ACTION && (v & 255) > KB_GRAVE_ESCAPE) ||
            (m == KB_FX_LAYER && v >= KB_LAYERS) || (m == KB_FX_HOST_LED && v >= 5) ||
            (m == KB_FX_MACRO && v >= KB_MACROS) || (m == KB_FX_CONSUMER && (!v || v > 1023)) ||
            (m == KB_FX_MODIFIERS && (!v || v > 255)))
            return -KB_EINVAL;
        if (t == KB_FX_TAP || t == KB_FX_DOUBLE_TAP) {
            if (threshold < 20 || threshold > 2000)
                return -KB_EINVAL;
        } else if (t == KB_FX_HELD) {
            if (threshold < 20 || threshold > 60000)
                return -KB_EINVAL;
        } else if (threshold)
            return -KB_EINVAL;
    }
    return 0;
}
void kb_fx_clear(struct kb_fx *f)
{
    memset(f->instances, 0, sizeof(f->instances));
    f->tap_valid = 0;
    f->victim_cursor = 0;
}
int kb_fx_set(struct kb_fx *f, const uint8_t *p, size_t size)
{
    if (!f || kb_fx_validate(p, size))
        return -KB_EINVAL;
    memcpy(f->program, p, size);
    kb_fx_clear(f);
    f->epoch_ms = f->now_ms;
    return 0;
}
unsigned kb_fx_interval(const struct kb_fx *f)
{
    return enabled(f) ? u16(f->program + 8) : 100;
}
static int matches(const struct kb_fx *f, const uint8_t *r, const struct kb_event *e,
                   unsigned trigger, unsigned aux)
{
    unsigned v = u16(r + 2), m = r[1];
    unsigned layers = key_trigger(trigger) ? e->layers : f->layers;
    unsigned modifiers = key_trigger(trigger) ? e->modifiers : f->mods;
    if ((layers & (r[30] & 15u)) != (r[30] & 15u) || (layers & (r[30] >> 4)))
        return 0;
    if (r[28] & KB_FX_ANY_MODS) {
        if (!(modifiers & r[31]))
            return 0;
    } else if (r[28] & KB_FX_NO_MODS) {
        if (modifiers & r[31])
            return 0;
    } else if ((modifiers & r[31]) != r[31])
        return 0;
    if (m == KB_FX_ANY)
        return 1;
    if (m == KB_FX_KEY)
        return e->key == v;
    if (m == KB_FX_USAGE)
        return (e->action == KB_KEY || e->action == KB_MOD_TAP || e->action == KB_LAYER_TAP ||
                e->action == KB_GRAVE_ESCAPE) &&
               e->code == v;
    if (m == KB_FX_CONSUMER)
        return e->action == KB_CONSUMER && e->code == v;
    if (m == KB_FX_ACTION)
        return e->action == (v & 255) && ((v >> 8) == 255 || e->parameter == (v >> 8));
    if (m == KB_FX_LAYER)
        return trigger == KB_FX_LAYER_ENTER || trigger == KB_FX_LAYER_LEAVE
                   ? aux == v
                   : !!((key_trigger(trigger) ? e->layers : f->layers) & (1u << v));
    if (m == KB_FX_HOST_LED)
        return trigger == KB_FX_LED_ON || trigger == KB_FX_LED_OFF ? aux == v
                                                                   : !!(f->leds & (1u << v));
    if (m == KB_FX_MACRO)
        return trigger == KB_FX_MACRO_START || trigger == KB_FX_MACRO_END
                   ? e->parameter == v
                   : f->keyboard && f->keyboard->macro_running && f->keyboard->macro_index == v;
    if (m == KB_FX_MODIFIERS)
        return trigger == KB_FX_MODS_ON || trigger == KB_FX_MODS_OFF
                   ? !!(aux & v)
                   : !!((key_trigger(trigger) ? e->modifiers : f->mods) & v);
    return 0;
}
static void activate(struct kb_fx *f, unsigned rule, unsigned key, uint32_t now)
{
    struct kb_fx_instance *chosen = NULL, *oldest = &f->instances[f->victim_cursor];
    const uint8_t *r = f->program + 16 + rule * 32;
    for (unsigned step = 0; step < KB_FX_INSTANCES; ++step) {
        unsigned i = (f->victim_cursor + step) % KB_FX_INSTANCES;
        struct kb_fx_instance *s = &f->instances[i];
        if (s->active && now - s->started >= u16(f->program + 16 + s->rule * 32 + 14))
            s->active = 0;
        if ((r[28] & KB_FX_SINGLE) && s->active && s->rule == rule) {
            chosen = s;
            break;
        }
        if (!s->active && !chosen)
            chosen = s;
        if (now - s->started > now - oldest->started)
            oldest = s;
    }
    if (!chosen) {
        chosen = oldest;
        if (f->dropped != UINT32_MAX)
            ++f->dropped;
    }
    f->victim_cursor = (uint8_t)((chosen - f->instances + 1) % KB_FX_INSTANCES);
    *chosen = (struct kb_fx_instance){now, (uint8_t)rule, (uint8_t)key, 1, 0};
}
static void event(struct kb_fx *f, unsigned trigger, const struct kb_event *e, unsigned aux)
{
    if (!enabled(f))
        return;
    for (unsigned i = 0; i < f->program[5]; ++i) {
        const uint8_t *r = f->program + 16 + i * 32;
        if (r[0] != trigger || !matches(f, r, e, trigger, aux))
            continue;
        unsigned threshold = u16(r + 18);
        if ((trigger == KB_FX_TAP || trigger == KB_FX_DOUBLE_TAP) && e->duration_ms > threshold)
            continue;
        if (trigger == KB_FX_DOUBLE_TAP && (!(f->tap_valid & (UINT64_C(1) << e->key)) ||
                                            e->now_ms - f->tap_ms[e->key] > threshold))
            continue;
        activate(f, i, e->key, e->now_ms);
    }
}
void kb_fx_keyboard_event(void *context, const struct kb_event *e)
{
    struct kb_fx *f = context;
    if (!f || !e)
        return;
    if (e->type == KB_EVENT_RESET) {
        kb_fx_clear(f);
        return;
    }
    unsigned trigger = e->type == KB_EVENT_PRESS         ? KB_FX_PRESS
                       : e->type == KB_EVENT_RELEASE     ? KB_FX_RELEASE
                       : e->type == KB_EVENT_TAP         ? KB_FX_TAP
                       : e->type == KB_EVENT_HOLD        ? KB_FX_HOLD_DECIDED
                       : e->type == KB_EVENT_MACRO_START ? KB_FX_MACRO_START
                       : e->type == KB_EVENT_MACRO_END   ? KB_FX_MACRO_END
                                                         : 255;
    if (trigger == 255 || e->key >= KB_KEYS)
        return;
    event(f, trigger, e, 0);
    if (e->type == KB_EVENT_TAP) {
        event(f, KB_FX_DOUBLE_TAP, e, 0);
        if (e->duration_ms <= 2000) {
            f->tap_valid |= UINT64_C(1) << e->key;
            f->tap_ms[e->key] = e->now_ms;
        } else
            f->tap_valid &= ~(UINT64_C(1) << e->key);
    }
}
static void context_event(struct kb_fx *f, unsigned trigger, unsigned aux, uint32_t now)
{
    const struct kb_event e = {.now_ms = now, .key = KB_FX_NO_KEY};
    event(f, trigger, &e, aux);
}
void kb_fx_observe(struct kb_fx *f, const struct keyboard *k, unsigned layers, unsigned leds,
                   int connected, uint32_t last_activity, uint32_t now)
{
    if (!f || !k)
        return;
    f->keyboard = k;
    f->now_ms = now;
    unsigned previous_layers = f->layers, previous_leds = f->leds, previous_mods = f->mods;
    unsigned previous_caps = f->caps, previous_connected = f->connected, previous_idle = f->idle;
    f->layers = (uint8_t)layers;
    f->leds = (uint8_t)(leds & 31);
    f->mods = k->current.nkro[0];
    f->caps = k->caps_word;
    f->connected = (uint8_t) !!connected;
    unsigned held = 0;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        held |= k->key[i].stable;
        if ((f->tap_valid & (UINT64_C(1) << i)) && now - f->tap_ms[i] > 2000)
            f->tap_valid &= ~(UINT64_C(1) << i);
    }
    unsigned idle_ms = u16(f->program + 10);
    f->idle = idle_ms && !held && now - last_activity >= idle_ms;
    if (!f->context_valid) {
        f->epoch_ms = now;
        f->context_valid = 1;
    }
    for (unsigned i = 0; i < KB_LAYERS; ++i) {
        unsigned bit = 1u << i;
        if ((layers ^ previous_layers) & bit) {
            if (layers & bit)
                f->layer_ms[i] = now;
            context_event(f, layers & bit ? KB_FX_LAYER_ENTER : KB_FX_LAYER_LEAVE, i, now);
        }
    }
    for (unsigned i = 0; i < 5; ++i) {
        unsigned bit = 1u << i;
        if ((f->leds ^ previous_leds) & bit) {
            if (f->leds & bit)
                f->led_ms[i] = now;
            context_event(f, f->leds & bit ? KB_FX_LED_ON : KB_FX_LED_OFF, i, now);
        }
    }
    if (f->mods != previous_mods) {
        f->mods_ms = now;
        if (f->mods & ~previous_mods)
            context_event(f, KB_FX_MODS_ON, f->mods & ~previous_mods, now);
        if (previous_mods & ~f->mods)
            context_event(f, KB_FX_MODS_OFF, previous_mods & ~f->mods, now);
    }
    if (f->caps != previous_caps) {
        if (f->caps)
            f->caps_ms = now;
        context_event(f, f->caps ? KB_FX_CAPS_ON : KB_FX_CAPS_OFF, 0, now);
    }
    if (f->connected != previous_connected)
        context_event(f, f->connected ? KB_FX_CONNECT : KB_FX_DISCONNECT, 0, now);
    if (f->idle != previous_idle)
        context_event(f, f->idle ? KB_FX_IDLE : KB_FX_WAKE, 0, now);
    for (unsigned i = 0; i < KB_FX_INSTANCES; ++i) {
        struct kb_fx_instance *s = &f->instances[i];
        if (s->active && now - s->started >= u16(f->program + 16 + s->rule * 32 + 14))
            s->active = 0;
    }
    if (!connected)
        kb_fx_clear(f);
}
static unsigned interpolate(unsigned a, unsigned b, unsigned t)
{
    return (a * (255 - t) + b * t) / 255;
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
static unsigned blend(unsigned dst, unsigned src, unsigned alpha, unsigned mode)
{
    if (mode == KB_FX_REPLACE)
        return interpolate(dst, src, alpha);
    unsigned v = src * alpha / 255;
    if (mode == KB_FX_ADD)
        return dst + v > 255 ? 255 : dst + v;
    if (mode == KB_FX_MAX)
        return dst > v ? dst : v;
    return dst * (255 - alpha + v) / 255;
}
static void paint(const uint8_t *r, unsigned source, uint32_t age, struct nk60_rgb out[KB_KEYS])
{
    unsigned duration = u16(r + 14), period = u16(r + 16),
             raw_phase = (unsigned)((uint64_t)(age % period) * 255 / period), phase = raw_phase;
    if (r[28] & KB_FX_REVERSE)
        phase = (r[5] == KB_FX_PULSE || r[5] == KB_FX_BREATHE) ? (phase + 128) & 255 : 255 - phase;
    uint64_t mask = r[4] == KB_FX_ALL    ? (UINT64_C(1) << KB_KEYS) - 1
                    : r[4] == KB_FX_SELF ? source < KB_KEYS ? UINT64_C(1) << source : 0
                                         : u64(r + 20);
    struct kb_key_position origin = {0};
    if (source < KB_KEYS && r[5] == KB_FX_RIPPLE)
        kb_layout_position(source, &origin);
    unsigned first = r[4] == KB_FX_SELF && source < KB_KEYS ? source : 0;
    unsigned last = r[4] == KB_FX_SELF && source < KB_KEYS ? source + 1 : KB_KEYS;
    for (unsigned key = first; key < last; ++key) {
        if (!(mask & (UINT64_C(1) << key)))
            continue;
        unsigned strength = 255, t = 0;
        struct nk60_rgb color = {r[8], r[9], r[10]};
        if (r[5] == KB_FX_FADE) {
            t = duration ? ((uint64_t)age * 255 / duration) : raw_phase;
            strength = r[28] & KB_FX_REVERSE ? t : 255 - t;
        } else if (r[5] == KB_FX_PULSE) {
            t = phase < 128 ? phase * 2 : (255 - phase) * 2;
            strength = 255 - t;
        } else if (r[5] == KB_FX_BREATHE) {
            t = phase < 128 ? phase * 2 : (255 - phase) * 2;
            strength = t;
        } else if (r[5] == KB_FX_RAINBOW)
            color = wheel(phase * 768 / 256);
        else if (r[5] == KB_FX_WAVE) {
            struct kb_key_position p;
            kb_layout_position(key, &p);
            unsigned position = r[28] & KB_FX_VERTICAL ? p.y * 2 + p.height : p.x * 2 + p.width;
            unsigned extent = r[28] & KB_FX_VERTICAL ? KB_LAYOUT_HEIGHT : KB_LAYOUT_WIDTH;
            t = (phase + position * 256 / (2 * extent)) & 255;
            t = t < 128 ? t * 2 : (255 - t) * 2;
        } else if (r[5] == KB_FX_RIPPLE) {
            struct kb_key_position p;
            kb_layout_position(key, &p);
            int dx = (int)(p.x * 2 + p.width) - (int)(origin.x * 2 + origin.width);
            int dy = (int)(p.y * 2 + p.height) - (int)(origin.y * 2 + origin.height);
            unsigned distance = (unsigned)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
            unsigned radius = phase * 160 / 255,
                     diff = distance > radius ? distance - radius : radius - distance;
            strength = diff >= 16 ? 0 : (16 - diff) * 255 / 16;
            t = phase;
        }
        if (r[5] != KB_FX_RAINBOW) {
            color.r = (uint8_t)interpolate(r[8], r[11], t);
            color.g = (uint8_t)interpolate(r[9], r[12], t);
            color.b = (uint8_t)interpolate(r[10], r[13], t);
        }
        unsigned alpha = strength * r[29] / 255;
        out[key].r = (uint8_t)blend(out[key].r, color.r, alpha, r[6]);
        out[key].g = (uint8_t)blend(out[key].g, color.g, alpha, r[6]);
        out[key].b = (uint8_t)blend(out[key].b, color.b, alpha, r[6]);
    }
}
static void state_paint(struct kb_fx *f, const uint8_t *r, unsigned key, uint32_t now,
                        struct nk60_rgb out[KB_KEYS])
{
    const struct keyboard *k = f->keyboard;
    struct kb_event e = {
        .now_ms = now, .key = (uint8_t)key, .layers = f->layers, .modifiers = f->mods};
    uint32_t started = f->epoch_ms;
    unsigned t = r[0], v = u16(r + 2);
    if (t == KB_FX_HELD) {
        if (!k || key >= KB_KEYS || !k->key[key].active || !k->key[key].stable)
            return;
        const struct kb_key_state *s = &k->key[key];
        if (now - s->pressed_ms < u16(r + 18))
            return;
        started = s->pressed_ms + u16(r + 18);
        e.action = s->origin_type;
        e.parameter = s->action.parameter;
        e.code = s->action.code;
    } else if (t == KB_FX_LAYER_ACTIVE) {
        unsigned selected = r[1] == KB_FX_LAYER ? v : 0;
        if (r[1] != KB_FX_LAYER)
            for (unsigned i = 0; i < KB_LAYERS; ++i)
                if (f->layers & (1u << i))
                    selected = i;
        if (!(f->layers & (1u << selected)))
            return;
        started = f->layer_ms[selected];
    } else if (t == KB_FX_LED_ACTIVE) {
        unsigned selected = r[1] == KB_FX_HOST_LED ? v : 0;
        if (r[1] != KB_FX_HOST_LED)
            while (selected < 5 && !(f->leds & (1u << selected)))
                ++selected;
        if (selected == 5 || !(f->leds & (1u << selected)))
            return;
        started = f->led_ms[selected];
    } else if (t == KB_FX_CAPS_ACTIVE) {
        if (!f->caps)
            return;
        started = f->caps_ms;
    } else if (t == KB_FX_MACRO_ACTIVE) {
        if (!k || !k->macro_running)
            return;
        e.key = k->macro_key;
        e.action = KB_MACRO;
        e.parameter = k->macro_index;
        started = k->macro_started_ms;
    } else if (t == KB_FX_MODS_ACTIVE) {
        if (!f->mods)
            return;
        started = f->mods_ms;
    }
    if (!matches(f, r, &e, t, 0))
        return;
    uint32_t age = now - started;
    if (u16(r + 14) && age >= u16(r + 14))
        return;
    paint(r, e.key, age, out);
}
static unsigned instance_order(struct kb_fx *f, uint32_t now, uint8_t order[KB_FX_INSTANCES])
{
    unsigned count = 0;
    for (unsigned step = 0; step < KB_FX_INSTANCES; ++step) {
        unsigned index = (f->victim_cursor + step) % KB_FX_INSTANCES;
        struct kb_fx_instance *s = &f->instances[index];
        if (!s->active)
            continue;
        uint32_t age = now - s->started;
        if (age >= u16(f->program + 16 + s->rule * 32 + 14)) {
            s->active = 0;
            continue;
        }
        unsigned at = count;
        while (at && age > now - f->instances[order[at - 1]].started) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = (uint8_t)index;
        ++count;
    }
    return count;
}
void kb_fx_render(struct kb_fx *f, uint32_t now, struct nk60_rgb out[KB_KEYS])
{
    if (!enabled(f))
        return;
    if (u16(f->program + 6) & KB_FX_DARK_BASE)
        memset(out, 0, sizeof(*out) * KB_KEYS);
    uint8_t order[KB_FX_INSTANCES];
    unsigned count = instance_order(f, now, order);
    for (unsigned rule = 0; rule < f->program[5]; ++rule) {
        const uint8_t *r = f->program + 16 + rule * 32;
        if (state_trigger(r[0])) {
            if (r[0] == KB_FX_HELD && r[4] == KB_FX_SELF) {
                for (unsigned key = 0; key < KB_KEYS; ++key)
                    state_paint(f, r, key, now, out);
            } else if (r[0] == KB_FX_HELD) {
                /* A wide held effect follows one most recently pressed matching
                 * source. This bounds a frame to at most (rules+instances)*61
                 * painted pixels, even with all61 switches held. */
                unsigned selected = KB_FX_NO_KEY;
                uint32_t youngest = UINT32_MAX;
                const struct keyboard *k = f->keyboard;
                if (k)
                    for (unsigned key = 0; key < KB_KEYS; ++key) {
                        const struct kb_key_state *s = &k->key[key];
                        uint32_t age = now - s->pressed_ms;
                        struct kb_event e = {.key = (uint8_t)key,
                                             .action = s->origin_type,
                                             .parameter = s->action.parameter,
                                             .code = s->action.code,
                                             .layers = f->layers,
                                             .modifiers = f->mods};
                        if (s->active && s->stable && age >= u16(r + 18) && age < youngest &&
                            matches(f, r, &e, KB_FX_HELD, 0)) {
                            selected = key;
                            youngest = age;
                        }
                    }
                if (selected < KB_KEYS)
                    state_paint(f, r, selected, now, out);
            } else
                state_paint(f, r, KB_FX_NO_KEY, now, out);
        } else
            for (unsigned i = 0; i < count; ++i) {
                struct kb_fx_instance *s = &f->instances[order[i]];
                if (!s->active || s->rule != rule)
                    continue;
                uint32_t age = now - s->started;
                if (age >= u16(r + 14)) {
                    s->active = 0;
                    continue;
                }
                paint(r, s->key, age, out);
            }
    }
}
