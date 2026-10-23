/* Copyright (C) 2026 NOS contributors. */
#include "nlp/input_device.h"
#include <string.h>
static void disconnect(void *context, uint32_t session)
{
    struct nlp_input_device *d = context;
    for (unsigned i = 0; i < NLP_SESSIONS; ++i)
        if (d->subscription[i] == session) {
            d->subscription[i] = d->binding[i] = 0;
        }
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_input_device *d = context;
    unsigned index = (unsigned)(b - b->endpoint->bindings);
    if (index >= NLP_SESSIONS || !(b->permissions & NLP_PERMISSION_OBSERVE_INPUT))
        return -NLP_DENIED;
    if (op == NLP_INPUT_GET_CONTROLS && size == 12 && !nlp_get16(in + 10) && *used >= 12) {
        if (nlp_get32(in) && nlp_get32(in) != 1)
            return -NLP_STALE;
        uint32_t cursor = nlp_get32(in + 4);
        unsigned count = 0, maximum = nlp_get16(in + 8);
        size_t capacity = *used;
        *used = 12;
        if (cursor > d->count || !maximum || (!nlp_get32(in) && cursor))
            return -NLP_INVALID;
        while (cursor < d->count && count < maximum && capacity - *used >= 16) {
            const struct nlp_control *c = &d->controls[cursor++];
            uint8_t *p = out + *used;
            memset(p, 0, 16);
            nlp_put16(p, c->id);
            p[2] = c->type;
            nlp_put32(p + 4, c->type == 1 ? 0 : (uint32_t)-32768);
            nlp_put32(p + 8, c->type == 1 ? 1 : 32767);
            nlp_put16(p + 12, c->resolution);
            *used += 16;
            ++count;
        }
        nlp_put32(out, 1);
        nlp_put32(out + 4, cursor < d->count ? cursor : 0);
        nlp_put16(out + 8, (uint16_t)count);
        nlp_put16(out + 10, cursor < d->count ? 1 : 0);
        return 0;
    }
    if (op == NLP_INPUT_GET_STATE && !size && *used >= 16) {
        memset(out, 0, 16);
        nlp_put32(out, d->sequence);
        nlp_put64(out + 4, d->endpoint->monotonic_ms * 1000);
        nlp_put16(out + 12, d->buttons);
        *used = 16;
        return 0;
    }
    if (op == NLP_INPUT_SUBSCRIBE && size == 4 && *used >= 4) {
        unsigned lease = nlp_get16(in), refresh = nlp_get16(in + 2);
        if (lease < 100 || lease > 5000 || !refresh || refresh > lease / 3 ||
            !(b->session.reliable_mask & 2))
            return -NLP_INVALID;
        d->subscription[index] = b->session.id;
        d->refresh[index] = refresh;
        d->last[index] = d->endpoint->now_ms - refresh;
        memcpy(out, in, 4);
        *used = 4;
        return 0;
    }
    if (op == NLP_INPUT_BIND && size == 16) {
        if (!(b->permissions & NLP_PERMISSION_BIND_INPUT))
            return -NLP_DENIED;
        if (!nlp_get32(in) || nlp_get64(in + 4) != d->endpoint->config.boot_id ||
            !nlp_get16(in + 12) || (nlp_get16(in + 12) & ~(d->button_mask | d->encoder_mask)) ||
            nlp_get16(in + 14))
            return -NLP_INVALID;
        if (d->subscription[index] != b->session.id)
            return -NLP_CONFLICT;
        uint16_t encoders = nlp_get16(in + 12) & d->encoder_mask;
        for (unsigned i = 1; i < NLP_SESSIONS; ++i)
            if (i != index && d->binding[i] && (d->mask[i] & encoders) &&
                d->endpoint->bindings[i].session.active)
                return -NLP_CONFLICT;
        d->binding[index] = nlp_get32(in);
        d->mask[index] = nlp_get16(in + 12);
        *used = 0;
        return 0;
    }
    if (op == NLP_INPUT_UNBIND && size == 4) {
        if (!(b->permissions & NLP_PERMISSION_BIND_INPUT))
            return -NLP_DENIED;
        if (nlp_get32(in) != d->binding[index])
            return -NLP_STALE;
        d->binding[index] = 0;
        *used = 0;
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_input_device_register(struct nlp_input_device *d, struct nlp_endpoint *e, uint16_t id,
                              const struct nlp_control *controls, unsigned count)
{
    struct nlp_capability c = {NLP_UUID_INPUT, id, 1,         0, 1, NLP_PERMISSION_OBSERVE_INPUT, 1,
                               call,           d,  disconnect};
    if (!d || !e || !controls || !count || count > 16)
        return -NLP_INVALID;
    memset(d, 0, sizeof(*d));
    d->endpoint = e;
    d->count = (uint8_t)count;
    d->sequence = 1;
    unsigned seen = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct nlp_control *p = &controls[i];
        if (p->id >= 16 || (seen & (1u << p->id)) || !p->resolution ||
            (p->type != 1 && p->type != 2) || (p->type == 1 && p->resolution != 1))
            return -NLP_INVALID;
        seen |= 1u << p->id;
        if (p->type == 1)
            d->button_mask |= (uint16_t)(1u << p->id);
        else
            d->encoder_mask |= (uint16_t)(1u << p->id);
        d->controls[i] = *p;
    }
    return nlp_endpoint_add(e, &c);
}
int nlp_input_device_buttons(struct nlp_input_device *d, uint16_t buttons)
{
    if (!d || (buttons & ~d->button_mask))
        return -NLP_INVALID;
    if (d->buttons != buttons) {
        if (d->sequence == UINT32_MAX)
            return -NLP_RANGE;
        ++d->sequence;
        d->buttons = buttons;
        for (unsigned i = 1; i < NLP_SESSIONS; ++i)
            d->last[i] = d->endpoint->now_ms - d->refresh[i];
    }
    return 0;
}
int nlp_input_device_rotate(struct nlp_input_device *d, uint16_t control, int16_t delta)
{
    if (!d || control >= 16 || !(d->encoder_mask & (1u << control)) || !delta)
        return -NLP_INVALID;
    if (d->relative_sequence == UINT32_MAX)
        return -NLP_RANGE;
    struct nlp_binding *target = NULL;
    /* Relative increments have one explicitly bound owner, avoiding partial fan-out retries. */
    for (unsigned i = 1; i < NLP_SESSIONS; ++i)
        if (d->binding[i] && (d->mask[i] & (1u << control)) &&
            d->endpoint->bindings[i].session.active &&
            d->subscription[i] == d->endpoint->bindings[i].session.id) {
            if (target)
                return -NLP_CONFLICT;
            target = &d->endpoint->bindings[i];
        }
    if (!target)
        return -NLP_NOT_FOUND;
    uint8_t payload[16];
    nlp_put32(payload, d->relative_sequence + 1);
    nlp_put64(payload + 4, d->endpoint->monotonic_ms * 1000);
    nlp_put16(payload + 12, control);
    nlp_put16(payload + 14, (uint16_t)delta);
    int error = nlp_binding_event(target, NLP_INPUT_RELATIVE, payload, 16);
    if (!error)
        ++d->relative_sequence;
    return error;
}
void nlp_input_device_poll(struct nlp_input_device *d)
{
    if (!d)
        return;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i) {
        struct nlp_binding *b = &d->endpoint->bindings[i];
        if (!b->session.active || d->subscription[i] != b->session.id || !d->refresh[i] ||
            d->endpoint->now_ms - d->last[i] < d->refresh[i])
            continue;
        if (d->sequence == UINT32_MAX) {
            nlp_session_close(&b->session, NLP_RANGE);
            continue;
        }
        uint8_t payload[16];
        nlp_put32(payload, d->sequence + 1);
        nlp_put64(payload + 4, d->endpoint->monotonic_ms * 1000);
        nlp_put16(payload + 12, d->buttons);
        nlp_put16(payload + 14, 0);
        if (!nlp_binding_event(b, NLP_INPUT_BUTTON_STATE, payload, 16)) {
            ++d->sequence;
            d->last[i] = d->endpoint->now_ms;
        }
    }
}
