/* Copyright (C) 2026 NOS contributors. Strict, bounded BT/Wi-Fi profile. */
#include "nlp/radio.h"
#include <string.h>
#define HAS(n) (1u << (n))
static void disconnect(void *context, uint32_t session)
{
    struct nlp_radio *r = context;
    if (r->owner == session) {
        r->cancel(r->context);
        r->owner = r->job = 0;
        r->state = 0;
    }
}
static int parse(const uint8_t *in, size_t size, struct nlp_radio_params *p)
{
    struct nlp_tlv_reader reader = {in, size, 0, 0, 0};
    struct nlp_tlv t;
    int next;
    memset(p, 0, sizeof(*p));
    while ((next = nlp_tlv_next(&reader, &t)) > 0) {
        unsigned tag = t.tag & 0x7fff;
        if (!tag || tag > 14 || tag == 7) {
            if (t.tag & 0x8000)
                return -NLP_UNSUPPORTED;
            continue;
        }
        p->present |= HAS(tag);
        switch (tag) {
        case 1:
            if (t.length != 4 || !(p->job = nlp_get32(t.value)))
                return -NLP_INVALID;
            break;
        case 2:
            if (t.length != 1)
                return -NLP_INVALID;
            p->role = t.value[0];
            break;
        case 3:
            if (t.length != 4)
                return -NLP_INVALID;
            p->duration_ms = nlp_get32(t.value);
            break;
        case 4:
            if (t.length != 2)
                return -NLP_INVALID;
            p->max_results = nlp_get16(t.value);
            break;
        case 5:
            if (t.length != 6)
                return -NLP_INVALID;
            memcpy(p->address, t.value, 6);
            break;
        case 6:
            if (t.length != 1 || t.value[0] > 1)
                return -NLP_INVALID;
            p->address_type = t.value[0];
            break;
        case 8:
            if (t.length != 16)
                return -NLP_INVALID;
            memcpy(p->source_uid, t.value, 16);
            break;
        case 9:
            if (t.length != 2 || !(p->source_cap = nlp_get16(t.value)))
                return -NLP_INVALID;
            break;
        case 10:
            if (t.length != 1 || t.value[0] > 1)
                return -NLP_INVALID;
            p->method = t.value[0];
            break;
        case 11:
            if (t.length != 4 || (p->passkey = nlp_get32(t.value)) > 999999)
                return -NLP_INVALID;
            break;
        case 12:
            if (!t.length || t.length > 32)
                return -NLP_INVALID;
            p->ssid_length = (uint8_t)t.length;
            memcpy(p->ssid, t.value, t.length);
            break;
        case 13:
            if (t.length < 8 || t.length > 63)
                return -NLP_INVALID;
            p->password_length = (uint8_t)t.length;
            memcpy(p->password, t.value, t.length);
            break;
        case 14:
            if (t.length != 1 || t.value[0] > 2)
                return -NLP_INVALID;
            p->security = t.value[0];
            break;
        }
    }
    return next < 0 ? next : 0;
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_radio *r = context;
    struct nlp_radio_params p;
    if (!(b->permissions & NLP_PERMISSION_RADIO_CONFIG) || !b->session.channel[1].cipher)
        return -NLP_DENIED;
    if ((op == 1 || op == 6) && !size) {
        size_t count = 0;
        uint8_t value[4];
        int error;
        if (op == 1) {
            nlp_put32(value, r->roles);
            error = nlp_tlv_add(out, *used, &count, 19, value, 4);
        } else {
            nlp_put32(value, r->job);
            error = nlp_tlv_add(out, *used, &count, 1, value, 4);
            value[0] = r->state;
            if (!error)
                error = nlp_tlv_add(out, *used, &count, 15, value, 1);
        }
        *used = count;
        return error;
    }
    int error = parse(in, size, &p);
    if (error) {
        nlp_secure_clear(&p, sizeof(p));
        return error;
    }
    uint32_t required = 0, allowed = 0;
    if (op == 2) {
        required = HAS(1) | HAS(3) | HAS(4);
        allowed = required | HAS(2);
        if (p.duration_ms < 100 || p.duration_ms > 60000 || !p.max_results || p.max_results > 32) {
            error = -NLP_RANGE;
            goto finished;
        }
    } else if (r->bluetooth) {
        if (op == 3) {
            required = HAS(1) | HAS(5) | HAS(6) | HAS(10);
            if (p.method)
                required |= HAS(11);
            allowed = required;
        } else if (op == 4)
            required = allowed = HAS(5) | HAS(6);
        else if (op == 5)
            required = allowed = HAS(8) | HAS(9);
        else {
            error = -NLP_UNSUPPORTED;
            goto finished;
        }
    } else {
        if (op == 3) {
            required = HAS(12) | HAS(14);
            if (p.security)
                required |= HAS(13);
            allowed = required;
        } else if (op == 4 || op == 5)
            allowed = HAS(1);
        else {
            error = -NLP_UNSUPPORTED;
            goto finished;
        }
    }
    if ((p.present & required) != required || (p.present & ~allowed) ||
        ((p.present & HAS(2)) && (p.role >= 32 || !(r->roles & (1u << p.role)))))
        error = -NLP_INVALID;
    else if (r->job && p.job)
        error = -NLP_BUSY;
    else
        error = r->control(r->context, op, &p);
    if (!error && p.job) {
        r->job = p.job;
        r->owner = b->session.id;
        r->results = 0;
        r->result_limit = (uint8_t)p.max_results;
        r->state = op == 2 ? 1 : op == 3 ? 2 : 3;
    }
finished:
    nlp_secure_clear(&p, sizeof(p));
    *used = 0;
    return error;
}
int nlp_radio_register(struct nlp_radio *r, struct nlp_endpoint *e, uint16_t id, int bluetooth,
                       uint32_t roles,
                       int (*control)(void *, unsigned, const struct nlp_radio_params *),
                       void (*cancel)(void *), void *context)
{
    static const uint8_t bt[16] = NLP_UUID_BLUETOOTH, wifi[16] = NLP_UUID_WIFI;
    struct nlp_capability c = {{0}, id,   1, 0,         1, NLP_PERMISSION_RADIO_CONFIG,
                               1,   call, r, disconnect};
    if (!r || !e || !roles || !control || !cancel)
        return -NLP_INVALID;
    memset(r, 0, sizeof(*r));
    r->endpoint = e;
    r->bluetooth = !!bluetooth;
    r->roles = roles;
    r->control = control;
    r->cancel = cancel;
    r->context = context;
    memcpy(c.type, bluetooth ? bt : wifi, 16);
    return nlp_endpoint_add(e, &c);
}
int nlp_radio_event(struct nlp_radio *r, unsigned op, const uint8_t *tlvs, size_t size)
{
    struct nlp_tlv_reader reader = {tlvs, size, 0, 0, 0};
    struct nlp_tlv t;
    int next;
    if (!r || op < 0x1001 || op > 0x1003 || size > NLP_PAYLOAD_MAX - 12)
        return -NLP_INVALID;
    while ((next = nlp_tlv_next(&reader, &t)) > 0)
        if ((t.tag & 0x7fff) == 11 || (t.tag & 0x7fff) == 13)
            return -NLP_DENIED;
    if (next < 0)
        return next;
    if (op == 0x1001 && r->results >= r->result_limit)
        return -NLP_NO_SPACE;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i) {
        struct nlp_binding *b = &r->endpoint->bindings[i];
        if (b->session.active && b->session.id == r->owner) {
            uint8_t bytes[NLP_PAYLOAD_MAX];
            nlp_put32(bytes, r->sequence + 1);
            nlp_put64(bytes + 4, r->endpoint->monotonic_ms * 1000);
            if (size)
                memcpy(bytes + 12, tlvs, size);
            int error = nlp_binding_event(b, op, bytes, size + 12);
            if (!error) {
                ++r->sequence;
                if (op == 0x1001)
                    ++r->results;
                if (op == 0x1003)
                    r->job = 0;
            }
            return error;
        }
    }
    return -NLP_DISCONNECTED;
}
