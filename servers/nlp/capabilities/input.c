/* Copyright (C) 2026 NOS contributors. */
#include "nlp/capabilities.h"
#include <string.h>
static struct nlp_input_source *find(struct nlp_inputs *in, const uint8_t uid[16], uint16_t cap)
{
    for (unsigned i = 0; i < NLP_INPUT_SOURCES; ++i)
        if (in->source[i].active && in->source[i].cap == cap && !memcmp(in->source[i].uid, uid, 16))
            return &in->source[i];
    return NULL;
}
int nlp_input_bind(struct nlp_inputs *in, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                   uint32_t binding, uint16_t controls, const uint16_t usages[16], uint32_t lease)
{
    struct nlp_input_source *s;
    if (!in || !uid || !boot || !cap || !binding || !controls || !usages || lease < 100 ||
        lease > 5000)
        return -NLP_INVALID;
    for (unsigned i = 0; i < 16; ++i)
        if ((controls & (1u << i)) && (usages[i] < 4 || usages[i] > 255))
            return -NLP_RANGE;
    s = find(in, uid, cap);
    if (!s)
        for (unsigned i = 0; i < NLP_INPUT_SOURCES; ++i)
            if (!in->source[i].active) {
                s = &in->source[i];
                break;
            }
    if (!s)
        return -NLP_NO_SPACE;
    memset(s, 0, sizeof(*s));
    memcpy(s->uid, uid, 16);
    memcpy(s->map, usages, sizeof(s->map));
    s->boot = boot;
    s->cap = cap;
    s->binding = binding;
    s->controls = controls;
    s->lease_ms = lease;
    s->active = s->blocked = 1;
    return 0;
}
int nlp_input_state(struct nlp_inputs *in, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                    const uint8_t state[16], uint32_t now)
{
    if (!in || !uid || !state)
        return -NLP_INVALID;
    struct nlp_input_source *s = find(in, uid, cap);
    if (!s || s->boot != boot)
        return -NLP_STALE;
    uint32_t seq = nlp_get32(state);
    uint16_t controls = nlp_get16(state + 12);
    if (!seq || nlp_get16(state + 14) || (controls & ~s->controls))
        return -NLP_INVALID;
    if (seq < s->seq)
        return -NLP_STALE;
    if (seq == s->seq && controls != s->buttons)
        return -NLP_CONFLICT;
    if (s->seq && now - s->last_ms >= s->lease_ms)
        s->blocked = 1;
    s->seq = seq;
    s->buttons = controls;
    s->last_ms = now;
    memset(s->bitmap, 0, 32);
    s->modifiers = 0;
    if (!controls)
        s->blocked = 0;
    if (!s->blocked)
        for (unsigned i = 0; i < 16; ++i)
            if (controls & (1u << i)) {
                unsigned usage = s->map[i];
                if (usage >= 0xe0 && usage <= 0xe7)
                    s->modifiers |= (uint8_t)(1u << (usage - 0xe0));
                else
                    s->bitmap[usage / 8] |= (uint8_t)(1u << (usage % 8));
            }
    return 0;
}
void nlp_input_drop(struct nlp_inputs *in, const uint8_t uid[16], uint16_t cap)
{
    if (!in || !uid)
        return;
    struct nlp_input_source *s = find(in, uid, cap);
    if (s)
        memset(s, 0, sizeof(*s));
}
void nlp_input_merge(struct nlp_inputs *in, uint32_t now, uint8_t nkro[33])
{
    if (!in || !nkro)
        return;
    for (unsigned i = 0; i < NLP_INPUT_SOURCES; ++i) {
        struct nlp_input_source *s = &in->source[i];
        if (!s->active)
            continue;
        if (!s->seq || now - s->last_ms >= s->lease_ms) {
            memset(s->bitmap, 0, 32);
            s->modifiers = 0;
            s->blocked = 1;
            continue;
        }
        nkro[0] |= s->modifiers;
        for (unsigned j = 0; j < 32; ++j)
            nkro[j + 1] |= s->bitmap[j];
    }
}

int nlp_input_relative(struct nlp_inputs *in, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                       const uint8_t event[16], uint32_t now,
                       int (*action)(void *, uint16_t, int16_t), void *context)
{
    if (!in || !uid || !event || !action)
        return -NLP_INVALID;
    struct nlp_input_source *s = find(in, uid, cap);
    if (!s || s->boot != boot || s->blocked || !s->seq || now - s->last_ms >= s->lease_ms)
        return -NLP_STALE;
    uint32_t seq = nlp_get32(event);
    unsigned control = nlp_get16(event + 12);
    int16_t delta = (int16_t)nlp_get16(event + 14);
    if (!seq || !delta || control >= 16 || !(s->controls & (1u << control)))
        return -NLP_INVALID;
    if (seq <= s->relative_seq)
        return -NLP_STALE;
    int error = action(context, (uint16_t)control, delta);
    if (!error)
        s->relative_seq = seq;
    return error;
}
