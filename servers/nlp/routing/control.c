/* Copyright (C) 2026 NOS contributors. Authenticated route control wire adapter. */
#include "nlp/endpoint.h"
#include "nlp/fabric.h"
#include <string.h>
int nlp_route_control(struct nlp_binding *b, uint32_t revision, unsigned action, uint16_t node,
                      const uint8_t target[16], const uint8_t child[16], uint32_t lease)
{
    uint8_t bytes[NLP_PAYLOAD_MAX], value[4];
    size_t size = 8;
    if (!b || !target || !child || !revision || action < 1 || action > 3 || node < 2 ||
        node >= 65534)
        return -NLP_INVALID;
    memset(bytes, 0, 8);
    nlp_put32(bytes, revision);
#define ADD(tag, p, n)                                                                             \
    do {                                                                                           \
        int error = nlp_tlv_add(bytes, sizeof(bytes), &size, tag, p, n);                           \
        if (error)                                                                                 \
            return error;                                                                          \
    } while (0)
    ADD(2, target, 16);
    nlp_put32(value, lease);
    ADD(12, value, 4);
    nlp_put32(value, revision);
    ADD(18, value, 4);
    value[0] = (uint8_t)action;
    ADD(19, value, 1);
    nlp_put16(value, node);
    ADD(20, value, 2);
    ADD(21, child, 16);
#undef ADD
    return nlp_session_send(&b->session, 0, NLP_CONTROL, NLP_OP_ROUTE_INSTALL, 0, bytes, size,
                            b->endpoint->now_ms);
}
int nlp_route_dispatch(struct nlp_binding *b, const struct nlp_frame *f)
{
    struct nlp_endpoint *e = b->endpoint;
    if (f->h.channel_id || f->length < 8 || !nlp_get32(f->payload) || nlp_get16(f->payload + 6) ||
        !(b->permissions & NLP_PERMISSION_COORDINATE) ||
        (!b->session.channel[0].cipher && !e->config.physical))
        return -NLP_DENIED;
    struct nlp_tlv_reader reader = {f->payload + 8, f->length - 8u, 0, 0, 0};
    struct nlp_tlv t;
    uint8_t uid[16], child[16], action = 0;
    uint16_t node = 0;
    uint32_t revision = 0, lease = 0;
    unsigned mask = 0;
    int next;
    while ((next = nlp_tlv_next(&reader, &t)) > 0) {
        switch (t.tag & 0x7fff) {
        case 2:
            if (t.length != 16)
                return -NLP_INVALID;
            memcpy(uid, t.value, 16);
            mask |= 1;
            break;
        case 12:
            if (t.length != 4)
                return -NLP_INVALID;
            lease = nlp_get32(t.value);
            mask |= 2;
            break;
        case 18:
            if (t.length != 4)
                return -NLP_INVALID;
            revision = nlp_get32(t.value);
            mask |= 4;
            break;
        case 19:
            if (t.length != 1)
                return -NLP_INVALID;
            action = t.value[0];
            mask |= 8;
            break;
        case 20:
            if (t.length != 2)
                return -NLP_INVALID;
            node = nlp_get16(t.value);
            mask |= 16;
            break;
        case 21:
            if (t.length != 16)
                return -NLP_INVALID;
            memcpy(child, t.value, 16);
            mask |= 32;
            break;
        default:
            if (t.tag & 0x8000)
                return -NLP_UNSUPPORTED;
            break;
        }
    }
    if (nlp_get16(f->payload + 4) > NLP_MTU_TOO_SMALL)
        return -NLP_INVALID;
    if (next < 0 || mask != 63 || nlp_get32(f->payload) != revision || !revision || action < 1 ||
        action > 3 || node < 2 || node >= 65534)
        return -NLP_INVALID;
    if (f->h.opcode == NLP_OP_ROUTE_RESULT) {
        if (!e->config.directory)
            return -NLP_UNSUPPORTED;
        int error = nlp_fabric_ack(e->config.directory, b->session.remote_node, revision, action,
                                   nlp_get16(f->payload + 4), e->now_ms);
        return error == -NLP_STALE ? 0 : error;
    }
    if (nlp_get16(f->payload + 4))
        return -NLP_INVALID;
    struct nlp_router *r = e->config.router;
    int result = 0;
    if (!r || !e->config.child_port)
        result = -NLP_UNSUPPORTED;
    else if (action == 1) {
        int port = e->config.child_port(e->config.context, child);
        if (port < 0 || (unsigned)port >= NLP_PORTS)
            result = -NLP_NOT_FOUND;
        else
            result = nlp_router_prepare(r, node, uid, (uint8_t)port, revision, e->now_ms, lease);
    } else {
        struct nlp_route *entry = NULL;
        for (unsigned i = 0; i < NLP_ROUTES; ++i)
            if ((r->routes[i].active || r->routes[i].prepared) && r->routes[i].node == node)
                entry = &r->routes[i];
        if (entry && (entry->revision > revision || memcmp(entry->uid, uid, 16)))
            result = -NLP_STALE;
        else if (action == 2) {
            if (!entry)
                result = -NLP_NOT_FOUND;
            else if (entry->revision != revision)
                result = -NLP_STALE;
            else {
                if (entry->prepared)
                    result = nlp_router_commit(r, revision);
                if (!result && lease && lease <= 60000)
                    entry->expires_ms = e->now_ms + lease;
            }
        } else {
            if (entry) {
                nlp_router_revoke(r, node);
                if (e->config.route_revoked)
                    e->config.route_revoked(e->config.context, node);
            }
        }
    }
    uint8_t response[NLP_PAYLOAD_MAX];
    memcpy(response, f->payload, f->length);
    nlp_put16(response + 4, (uint16_t)-result);
    return nlp_session_send(&b->session, 0, NLP_CONTROL, NLP_OP_ROUTE_RESULT, 0, response,
                            f->length, e->now_ms);
}
