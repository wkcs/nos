/* Copyright (C) 2026 NOS contributors. */
#include "nlp/capabilities.h"
#include "nlp/text.h"
#include <string.h>
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_log *l = context;
    unsigned i = (unsigned)(b - b->endpoint->bindings);
    if (i >= NLP_SESSIONS || !(b->permissions & NLP_PERMISSION_LOG_READ))
        return -NLP_DENIED;
    if ((op == NLP_LOG_SUBSCRIBE || op == NLP_LOG_SET_FILTER) && size >= 8) {
        unsigned count = in[1];
        uint16_t rate = nlp_get16(in + 4);
        if (!in[0] || (in[0] & ~63u) || count > 4 || size != 8 + count * 2u || nlp_get16(in + 2) ||
            !rate || rate > 16 || nlp_get16(in + 6) != 1)
            return -NLP_INVALID;
        l->mask[i] = in[0];
        l->component_count[i] = (uint8_t)count;
        l->rate[i] = rate;
        for (unsigned j = 0; j < count; ++j)
            l->components[i][j] = nlp_get16(in + 8 + j * 2);
        if (op == NLP_LOG_SUBSCRIBE) {
            l->subscription_id[i] = b->session.id;
            l->subscribed[i] = 1;
            l->cursor[i] = l->sequence > 4 ? l->sequence - 4 : 0;
            l->stamp[i] = l->endpoint->now_ms - 1000;
        }
        *used = 0;
        return 0;
    }
    if (op == NLP_LOG_UNSUBSCRIBE && !size) {
        l->subscribed[i] = 0;
        *used = 0;
        return 0;
    }
    if (op == NLP_LOG_GET_STATS && !size && *used >= 16) {
        nlp_put32(out, 4u * NLP_PAYLOAD_MAX);
        nlp_put32(out + 4, l->sequence < 4 ? l->sequence : 4);
        nlp_put32(out + 8, l->dropped);
        nlp_put32(out + 12, l->sequence);
        *used = 16;
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_log_register(struct nlp_log *l, struct nlp_endpoint *e, uint16_t id)
{
    struct nlp_capability c = {NLP_UUID_LOG, id, 1,   0, 1, NLP_PERMISSION_LOG_READ, 1,
                               call,         l,  NULL};
    if (!l || !e)
        return -NLP_INVALID;
    memset(l, 0, sizeof(*l));
    l->endpoint = e;
    return nlp_endpoint_add(e, &c);
}
int nlp_log_write(struct nlp_log *l, unsigned level, uint16_t component, const char *text,
                  size_t size, uint64_t stamp)
{
    if (!l || !l->endpoint || level > 5 || (!text && size))
        return -NLP_INVALID;
    if (l->sequence == UINT32_MAX)
        return -NLP_RANGE;
    struct nlp_log_record *r = &l->records[l->sequence % 4];
    if (r->seq)
        ++l->dropped;
    size_t prefix;
    int error = nlp_utf8_prefix((const uint8_t *)text, size, NLP_PAYLOAD_MAX - 24, &prefix);
    if (error)
        return error;
    uint8_t truncated = prefix < size;
    size = prefix;
    memset(r->bytes, 0, 24);
    nlp_put64(r->bytes, stamp);
    r->seq = ++l->sequence;
    nlp_put32(r->bytes + 8, r->seq);
    nlp_put32(r->bytes + 12, l->dropped);
    nlp_put16(r->bytes + 16, component);
    r->bytes[18] = (uint8_t)level;
    r->bytes[19] = truncated;
    nlp_put16(r->bytes + 20, (uint16_t)size);
    if (size)
        memcpy(r->bytes + 24, text, size);
    r->length = (uint16_t)(24 + size);
    return 0;
}
void nlp_log_poll(struct nlp_log *l)
{
    if (!l || !l->endpoint)
        return;
    struct nlp_endpoint *e = l->endpoint;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i) {
        struct nlp_binding *b = &e->bindings[i];
        if (!b->session.active || !b->capability || b->capability->context != l) {
            l->subscribed[i] = 0;
            continue;
        }
        if (l->subscription_id[i] != b->session.id)
            l->subscribed[i] = 0;
        if (!l->subscribed[i] || e->now_ms - l->stamp[i] < (1000u + l->rate[i] - 1) / l->rate[i])
            continue;
        if (l->sequence - l->cursor[i] > 4)
            l->cursor[i] = l->sequence - 4;
        while (l->cursor[i] < l->sequence) {
            struct nlp_log_record *r = &l->records[l->cursor[i] % 4];
            int component = !l->component_count[i];
            for (unsigned j = 0; j < l->component_count[i]; ++j)
                if (l->components[i][j] == nlp_get16(r->bytes + 16))
                    component = 1;
            if (!(l->mask[i] & (1u << r->bytes[18])) || !component) {
                ++l->cursor[i];
                continue;
            }
            uint8_t bytes[NLP_PAYLOAD_MAX];
            size_t length = r->length;
            memcpy(bytes, r->bytes, length);
            if (length > b->session.payload_limit) {
                size_t prefix;
                if (nlp_utf8_prefix(bytes + 24, length - 24, b->session.payload_limit - 24,
                                    &prefix))
                    break;
                length = prefix + 24;
                bytes[19] |= 1;
                nlp_put16(bytes + 20, (uint16_t)prefix);
            }
            if (!nlp_binding_event(b, NLP_LOG_RECORD, bytes, length)) {
                ++l->cursor[i];
                l->stamp[i] = e->now_ms;
            }
            break;
        }
    }
}
