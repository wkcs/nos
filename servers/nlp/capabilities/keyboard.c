/* Copyright (C) 2026 NOS contributors. */
#include "nlp/capabilities.h"
#include <string.h>
static unsigned binding_index(struct nlp_binding *b)
{
    return (unsigned)(b - b->endpoint->bindings);
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_keyboard *k = context;
    unsigned index = binding_index(b);
    if (index >= NLP_SESSIONS || !(b->permissions & NLP_PERMISSION_OBSERVE_INPUT))
        return -NLP_DENIED;
    if (op == NLP_KEYBOARD_GET_STATE && !size && *used >= 52) {
        memcpy(out, k->state, 52);
        *used = 52;
        return 0;
    }
    if (op == NLP_KEYBOARD_SUBSCRIBE && size == 4 && *used >= 4) {
        uint16_t lease = nlp_get16(in), refresh = nlp_get16(in + 2);
        if (lease < 100 || lease > 5000 || !refresh || refresh > lease / 3 ||
            !(b->session.reliable_mask & 2))
            return -NLP_INVALID;
        k->subscription_id[index] = b->session.id;
        k->lease[index] = lease;
        k->refresh[index] = refresh;
        k->subscribed[index] = 1;
        k->blocked[index] = 1;
        k->last[index] = k->endpoint->now_ms - refresh;
        memcpy(out, in, 4);
        *used = 4;
        return 0;
    }
    if (op == NLP_KEYBOARD_SET_LEDS) {
        if (!(b->permissions & NLP_PERMISSION_CONFIG_WRITE))
            return -NLP_DENIED;
        if (size != 4 || in[0] > 31 || in[1] || in[2] || in[3])
            return -NLP_INVALID;
        if (!k->set_leds)
            return -NLP_UNSUPPORTED;
        int error = k->set_leds(k->context, in[0]);
        *used = 0;
        return error;
    }
    if (op == NLP_KEYBOARD_SET_OUTPUT_OWNER) {
        if (!(b->permissions & NLP_PERMISSION_BIND_INPUT))
            return -NLP_DENIED;
        if (size != 20 || in[18] > 1 || in[19])
            return -NLP_INVALID;
        if (!k->set_owner)
            return -NLP_UNSUPPORTED;
        int error = k->set_owner(k->context, in, nlp_get16(in + 16), in[18]);
        *used = 0;
        return error;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_keyboard_register(struct nlp_keyboard *k, struct nlp_endpoint *e, uint16_t id)
{
    struct nlp_capability c = {
        NLP_UUID_KEYBOARD, id, 1, 0, 1, NLP_PERMISSION_OBSERVE_INPUT, 1, call, k, NULL};
    if (!k || !e)
        return -NLP_INVALID;
    memset(k, 0, sizeof(*k));
    k->endpoint = e;
    nlp_put16(k->state + 14, 32);
    return nlp_endpoint_add(e, &c);
}
void nlp_keyboard_update(struct nlp_keyboard *k, const uint8_t nkro[33], uint16_t consumer,
                         uint64_t stamp)
{
    if (!k || !nkro || consumer > 1023)
        return;
    if (memcmp(k->state + 16, nkro + 1, 32) || k->state[12] != nkro[0] ||
        nlp_get16(k->state + 48) != consumer) {
        if (k->sequence == UINT32_MAX) {
            nlp_endpoint_disconnect(k->endpoint);
            return;
        }
        for (unsigned i = 1; i < NLP_SESSIONS; ++i)
            if (k->subscribed[i] && k->sent_sequence[i] != k->sequence)
                k->blocked[i] = 1;
        nlp_put32(k->state, ++k->sequence);
    }
    nlp_put64(k->state + 4, stamp);
    k->state[12] = nkro[0];
    k->state[13] = 0;
    memcpy(k->state + 16, nkro + 1, 32);
    k->state[16] &= 0xf0;
    k->state[16 + 28] = 0;
    nlp_put16(k->state + 48, consumer);
}
void nlp_keyboard_poll(struct nlp_keyboard *k)
{
    if (!k || !k->endpoint)
        return;
    struct nlp_endpoint *e = k->endpoint;
    int all_up = !k->state[12] && !nlp_get16(k->state + 48);
    for (unsigned i = 0; i < 32; ++i)
        if (k->state[16 + i])
            all_up = 0;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i) {
        struct nlp_binding *b = &e->bindings[i];
        if (!b->session.active || !b->capability || b->capability->context != k) {
            k->subscribed[i] = 0;
            continue;
        }
        if (k->subscription_id[i] != b->session.id)
            k->subscribed[i] = 0;
        if (!k->subscribed[i] ||
            (e->now_ms - k->last[i] < k->refresh[i] && k->sent_sequence[i] == k->sequence))
            continue;
        uint8_t state[52];
        memcpy(state, k->state, 52);
        if (k->blocked[i]) {
            memset(state + 12, 0, 40);
            state[13] = 1;
            nlp_put16(state + 14, 32);
        }
        if (!nlp_binding_event(b, NLP_KEYBOARD_STATE, state, 52)) {
            k->last[i] = e->now_ms;
            k->sent_sequence[i] = k->sequence;
            if (all_up)
                k->blocked[i] = 0;
        } else if (e->now_ms - k->last[i] >= k->lease[i])
            k->blocked[i] = 1;
    }
}
