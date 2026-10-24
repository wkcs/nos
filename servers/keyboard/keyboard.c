/* Copyright (C) 2026 NOS contributors */
#include <string.h>
#include "keyboard.h"

static uint32_t elapsed(uint32_t now, uint32_t then)
{
    return now - then;
}
static void add_usage(uint8_t *bitmap, uint8_t *mods, unsigned usage)
{
    if (usage >= 0xe0 && usage <= 0xe7)
        *mods |= (uint8_t)(1u << (usage - 0xe0));
    else if (usage >= 4 && usage < 0xe0)
        bitmap[usage >> 3] |= (uint8_t)(1u << (usage & 7));
}

unsigned kb_active_layers(const struct keyboard *k)
{
    unsigned i, mask = 1u | k->toggled_layers;
    for (i = 0; i < KB_KEYS; ++i)
        if (k->key[i].active && !k->key[i].pending &&
            (k->key[i].action.type == KB_MOMENTARY || k->key[i].action.type == KB_LAYER_TAP))
            mask |= 1u << k->key[i].action.parameter;
    return mask;
}

static struct kb_action resolve(const struct keyboard *k, unsigned position)
{
    unsigned mask = kb_active_layers(k), l = k->config.layers;
    while (l--) {
        struct kb_action a = k->config.map[l][position];
        if ((mask & (1u << l)) && a.type != KB_TRANSPARENT)
            return a;
    }
    return (struct kb_action){KB_NONE, 0, 0};
}

static unsigned held_modifiers(const struct keyboard *);
void kb_set_event_handler(struct keyboard *k, kb_event_fn event, void *context)
{
    if (k) {
        k->event = event;
        k->event_context = context;
    }
}
static void notify(struct keyboard *k, unsigned type, unsigned key, uint32_t now)
{
    if (!k->event)
        return;
    struct kb_event e = {.now_ms = now, .type = (uint8_t)type, .key = (uint8_t)key};
    if (key < KB_KEYS) {
        const struct kb_key_state *s = &k->key[key];
        e.duration_ms = now - s->pressed_ms;
        e.action = s->origin_type;
        e.parameter = s->action.parameter;
        e.code = s->action.code;
    }
    if (type == KB_EVENT_MACRO_START || type == KB_EVENT_MACRO_END) {
        e.action = KB_MACRO;
        e.parameter = k->macro_index;
        e.duration_ms = now - k->macro_started_ms;
    }
    e.modifiers = (uint8_t)(held_modifiers(k) | k->oneshot);
    for (unsigned i = 0; i < KB_KEYS; ++i)
        if (k->key[i].stable && k->key[i].active && !k->key[i].pending)
            e.modifiers |= k->key[i].oneshot;
    e.layers = (uint8_t)kb_active_layers(k);
    k->event(k->event_context, &e);
}
static void logical_clear(struct keyboard *k)
{
    unsigned i;
    notify(k, KB_EVENT_RESET, 255, k->last_ms);
    for (i = 0; i < KB_KEYS; ++i) {
        k->key[i].active = k->key[i].pending = k->key[i].oneshot = 0;
        k->key[i].action = (struct kb_action){KB_NONE, 0, 0};
    }
    k->toggled_layers = k->oneshot = k->caps_word = 0;
    k->macro_running = k->macro_on = 0;
    k->macro_usage = 0;
}

static void build(const struct keyboard *k, struct kb_report *r, unsigned pulse,
                  unsigned pulse_mods)
{
    unsigned i, count = 0;
    uint8_t mods = (uint8_t)pulse_mods;
    uint16_t consumer = 0;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < KB_KEYS; ++i) {
        const struct kb_key_state *s = &k->key[i];
        if (!s->active || s->pending)
            continue;
        mods |= s->oneshot;
        switch (s->action.type) {
        case KB_KEY:
            add_usage(r->nkro + 1, &mods, s->action.code);
            break;
        case KB_MODIFIER:
        case KB_MOD_TAP:
            mods |= s->action.parameter;
            break;
        case KB_CONSUMER:
            if (!consumer)
                consumer = s->action.code;
            break;
        default:
            break;
        }
    }
    add_usage(r->nkro + 1, &mods, pulse);
    if (k->macro_on)
        add_usage(r->nkro + 1, &mods, k->macro_usage);
    r->boot[0] = r->nkro[0] = mods;
    r->consumer[0] = consumer;
    r->consumer[1] = consumer >> 8;
    for (i = 4; i < 0xe0; ++i)
        if (r->nkro[1 + (i >> 3)] & (1u << (i & 7))) {
            if (count < 6)
                r->boot[2 + count] = i;
            ++count;
        }
    if (count > 6)
        memset(r->boot + 2, 1, 6);
}

static int emit(struct keyboard *k, unsigned pulse, unsigned mods)
{
    struct kb_report r;
    build(k, &r, pulse, mods);
    if (!memcmp(r.boot, k->current.boot, sizeof(r.boot)) &&
        !memcmp(r.nkro, k->current.nkro, sizeof(r.nkro)) &&
        !memcmp(r.consumer, k->current.consumer, sizeof(r.consumer)))
        return 0;
    if (k->count == KB_REPORT_QUEUE) {
        ++k->overflows;
        logical_clear(k);
        k->head = k->count = 0;
        k->blocked = 1;
        memset(&r, 0, sizeof(r));
        r.sequence = ++k->sequence;
        k->current = k->reports[0] = r;
        k->count = 1;
        return -KB_ENOSPC;
    }
    r.sequence = ++k->sequence;
    k->current = r;
    k->reports[(k->head + k->count) % KB_REPORT_QUEUE] = r;
    ++k->count;
    return 0;
}

int kb_init(struct keyboard *k, const struct kb_config *config)
{
    if (!k || kb_config_validate(config))
        return -KB_EINVAL;
    memset(k, 0, sizeof(*k));
    k->config = *config;
    return 0;
}
const struct kb_report *kb_report_peek(const struct keyboard *k)
{
    return k && k->count ? &k->reports[k->head] : 0;
}
void kb_report_pop(struct keyboard *k)
{
    if (k && k->count) {
        k->head = (k->head + 1) % KB_REPORT_QUEUE;
        --k->count;
    }
}
void kb_release_all(struct keyboard *k)
{
    unsigned i;
    if (!k)
        return;
    logical_clear(k);
    for (i = 0; i < KB_KEYS; ++i)
        k->key[i].raw = k->key[i].stable = 0;
    k->head = k->count = 0;
    memset(&k->current, 0xff, sizeof(k->current));
    emit(k, 0, 0);
}
int kb_reconfigure(struct keyboard *k, const struct kb_config *config)
{
    if (!k || kb_config_validate(config))
        return -KB_EINVAL;
    kb_release_all(k);
    k->config = *config;
    k->blocked = 1;
    return 0;
}
uint32_t kb_take_local_events(struct keyboard *k)
{
    uint32_t events = k->local_events;
    k->local_events = 0;
    return events;
}

static void promote_pending(struct keyboard *k, uint32_t now)
{
    unsigned i;
    for (i = 0; i < KB_KEYS; ++i) {
        if (k->key[i].pending) {
            k->key[i].pending = 0;
            notify(k, KB_EVENT_HOLD, i, now);
        }
    }
}

static unsigned held_modifiers(const struct keyboard *k)
{
    unsigned mods = 0;
    for (unsigned i = 0; i < KB_KEYS; ++i) {
        const struct kb_key_state *state = &k->key[i];
        if (!state->stable)
            continue;
        struct kb_action a = state->active ? state->action : resolve(k, i);
        if (state->active && state->pending)
            continue;
        if (a.type == KB_MODIFIER || a.type == KB_MOD_TAP)
            mods |= a.parameter;
        if (a.type == KB_KEY && a.code >= KB_LCTRL && a.code <= KB_RGUI)
            mods |= 1u << (a.code - KB_LCTRL);
    }
    return mods;
}
static int press(struct keyboard *k, unsigned position, uint32_t now)
{
    struct kb_key_state *s = &k->key[position];
    s->action = resolve(k, position);
    s->origin_type = s->action.type;
    if (s->action.type == KB_GRAVE_ESCAPE)
        s->action = (struct kb_action){KB_KEY, 0, held_modifiers(k) & 0xaau ? KB_GRAVE : KB_ESCAPE};
    s->pressed_ms = now;
    s->active = 1;
    s->pending = s->action.type == KB_MOD_TAP || s->action.type == KB_LAYER_TAP;
    switch (s->action.type) {
    case KB_TOGGLE:
        k->toggled_layers ^= 1u << s->action.parameter;
        break;
    case KB_ONESHOT:
        k->oneshot |= s->action.parameter;
        break;
    case KB_KEY:
        if (k->caps_word && s->action.code < KB_LCTRL) {
            unsigned u = s->action.code;
            if ((u >= KB_A && u <= KB_Z) || u == KB_MINUS)
                s->oneshot |= 2u;
            else if (!((u >= KB_1 && u <= KB_0) || u == KB_BACKSPACE || u == KB_DELETE))
                k->caps_word = 0;
            k->caps_word_ms = now;
        }
        if (s->action.code < 0xe0) {
            s->oneshot |= k->oneshot;
            k->oneshot = 0;
        }
        break;
    case KB_MACRO:
        if (k->macro_running) {
            notify(k, KB_EVENT_PRESS, position, now);
            return -KB_EBUSY;
        }
        if (k->config.macro_length[s->action.parameter]) {
            k->macro_running = k->macro_on = 1;
            k->macro_index = s->action.parameter;
            k->macro_key = (uint8_t)position;
            k->macro_started_ms = now;
            notify(k, KB_EVENT_MACRO_START, position, now);
            k->macro_step = 0;
            k->macro_usage = k->config.macros[k->macro_index][0];
            k->macro_changed_ms = now;
        }
        break;
    case KB_LOCAL:
        if (s->action.parameter != KB_CAPS_WORD)
            k->local_events |= 1u << s->action.parameter;
        switch (s->action.parameter) {
        case KB_LIGHT_NEXT:
            k->config.rgb_mode = (k->config.rgb_mode + 1) % KB_RGB_MODES;
            break;
        case KB_LIGHT_UP:
            k->config.brightness = k->config.brightness > 239 ? 255 : k->config.brightness + 16;
            break;
        case KB_LIGHT_DOWN:
            k->config.brightness = k->config.brightness < 16 ? 0 : k->config.brightness - 16;
            break;
        case KB_NKRO_TOGGLE:
            k->config.nkro ^= 1;
            break;
        case KB_CAPS_WORD:
            k->caps_word ^= 1;
            k->caps_word_ms = now;
            break;
        }
        break;
    default:
        break;
    }
    notify(k, KB_EVENT_PRESS, position, now);
    return 0;
}

int kb_scan(struct keyboard *k, uint64_t pressed, uint32_t now)
{
    uint64_t down = 0, up = 0;
    unsigned i;
    int error = 0, result;
    if (!k || (pressed >> KB_KEYS))
        return -KB_EINVAL;
    if (k->clock_valid && elapsed(now, k->last_ms) > INT32_MAX)
        return -KB_EINVAL;
    k->clock_valid = 1;
    k->last_ms = now;
    if (k->caps_word && elapsed(now, k->caps_word_ms) >= 5000)
        k->caps_word = 0;
    if (k->blocked) {
        if (!pressed) {
            k->blocked = 0;
            for (i = 0; i < KB_KEYS; ++i)
                k->key[i].raw = k->key[i].stable = 0;
        }
        return 0;
    }
    for (i = 0; i < KB_KEYS; ++i) {
        struct kb_key_state *s = &k->key[i];
        unsigned raw = (unsigned)((pressed >> i) & 1);
        if (raw != s->raw) {
            s->raw = raw;
            s->changed_ms = now;
        }
        if (s->raw != s->stable && elapsed(now, s->changed_ms) >= k->config.debounce_ms) {
            s->stable = s->raw;
            if (s->stable)
                down |= (uint64_t)1 << i;
            else
                up |= (uint64_t)1 << i;
        }
        if (s->pending && elapsed(now, s->pressed_ms) >= k->config.tapping_ms) {
            s->pending = 0;
            notify(k, KB_EVENT_HOLD, i, now);
        }
    }
    /* Releases always use their press-time binding. A quick tap contributes a
     * queued down/up pulse even if the USB transport is temporarily busy. */
    for (i = 0; i < KB_KEYS; ++i)
        if (up & ((uint64_t)1 << i)) {
            struct kb_key_state *s = &k->key[i];
            unsigned tap = s->pending ? s->action.code : 0;
            unsigned mods = tap ? k->oneshot : 0;
            if (tap)
                k->oneshot = 0;
            if (s->active) {
                if (s->pending || (s->action.type != KB_MOD_TAP && s->action.type != KB_LAYER_TAP))
                    notify(k, KB_EVENT_TAP, i, now);
                notify(k, KB_EVENT_RELEASE, i, now);
            }
            s->active = s->pending = s->oneshot = 0;
            if (tap) {
                result = emit(k, tap, mods);
                if (result)
                    return result;
                result = emit(k, 0, 0);
                if (result)
                    return result;
            }
        }
    if (down) {
        uint64_t edges = down;
        /* Another switch promotes pending tap/holds before its layer lookup. */
        promote_pending(k, now);
        /* Bind layer keys first, including a simultaneous FN+key chord. */
        for (i = 0; i < KB_KEYS; ++i)
            if (down & ((uint64_t)1 << i)) {
                struct kb_action a = resolve(k, i);
                if (a.type == KB_MOMENTARY || a.type == KB_LAYER_TAP || a.type == KB_MOD_TAP ||
                    a.type == KB_TOGGLE) {
                    result = press(k, i, now);
                    if (result)
                        error = result;
                    down &= ~((uint64_t)1 << i);
                }
            }
        /* A simultaneous other key makes a newly pressed layer-tap a hold. */
        if (down || (edges & (edges - 1)))
            promote_pending(k, now);
        for (i = 0; i < KB_KEYS; ++i)
            if (down & ((uint64_t)1 << i)) {
                result = press(k, i, now);
                if (result)
                    error = result;
            }
    }
    /* Advance macros only after the previous edge has been consumed. This
     * prevents an arbitrary host stall from collapsing a macro down/up pair. */
    if (k->macro_running && !k->count && elapsed(now, k->macro_changed_ms) >= k->config.macro_ms) {
        k->macro_changed_ms = now;
        if (k->macro_on)
            k->macro_on = 0;
        else if (++k->macro_step < k->config.macro_length[k->macro_index]) {
            k->macro_usage = k->config.macros[k->macro_index][k->macro_step];
            k->macro_on = 1;
        } else {
            notify(k, KB_EVENT_MACRO_END, k->macro_key, now);
            k->macro_running = 0;
        }
    }
    result = emit(k, 0, 0);
    return result ? result : error;
}
