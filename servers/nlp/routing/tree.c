/* Copyright (C) 2026 NOS contributors. */
#include "nlp/router.h"
#include <string.h>
void nlp_router_init(struct nlp_router *r, const uint8_t uid[16])
{
    memset(r, 0, sizeof(*r));
    memcpy(r->uid, uid, 16);
    r->parent = NLP_NO_PORT;
}
int nlp_router_join(struct nlp_router *r, const uint8_t fabric[16], uint32_t epoch, uint16_t node,
                    uint8_t parent, const uint8_t *ancestors, unsigned count)
{
    if (!r || !fabric || !epoch || !node || node >= 65534 || count > 8 || (count && !ancestors) ||
        (node == 1 ? (count || parent != NLP_NO_PORT) : (!count || parent >= NLP_PORTS)))
        return -NLP_INVALID;
    for (unsigned i = 0; i < count; ++i) {
        if (!memcmp(ancestors + i * 16, r->uid, 16))
            return -NLP_CONFLICT;
        for (unsigned j = 0; j < i; ++j)
            if (!memcmp(ancestors + i * 16, ancestors + j * 16, 16))
                return -NLP_CONFLICT;
    }
    memset(r->routes, 0, sizeof(r->routes));
    memcpy(r->fabric, fabric, 16);
    r->epoch = epoch;
    r->node = node;
    r->parent = parent;
    r->depth = (uint8_t)count;
    r->revision = 0;
    if (count)
        memcpy(r->ancestors, ancestors, count * 16);
    return 0;
}
int nlp_router_prepare(struct nlp_router *r, uint16_t node, const uint8_t uid[16], uint8_t port,
                       uint32_t revision, uint32_t now, uint32_t lease)
{
    struct nlp_route *slot = NULL;
    if (!r || !r->epoch || !uid || node < 2 || node >= 65534 || node == r->node ||
        port >= NLP_PORTS || port == r->parent || !revision || revision < r->revision || !lease ||
        lease > INT32_MAX)
        return -NLP_INVALID;
    if (!memcmp(uid, r->uid, 16))
        return -NLP_CONFLICT;
    for (unsigned i = 0; i < r->depth; ++i)
        if (!memcmp(uid, r->ancestors[i], 16))
            return -NLP_CONFLICT;
    for (unsigned i = 0; i < NLP_ROUTES; ++i) {
        struct nlp_route *p = &r->routes[i];
        if ((p->active || p->prepared) && (p->node == node || !memcmp(p->uid, uid, 16))) {
            if (p->node != node || memcmp(p->uid, uid, 16) || p->port != port)
                return -NLP_CONFLICT;
            slot = p;
            break;
        }
        if (!p->active && !p->prepared)
            slot = p;
    }
    if (!slot)
        return -NLP_NO_SPACE;
    slot->node = node;
    slot->port = port;
    memcpy(slot->uid, uid, 16);
    slot->revision = revision;
    slot->expires_ms = now + lease;
    slot->prepared = 1;
    return 0;
}
int nlp_router_commit(struct nlp_router *r, uint32_t revision)
{
    int found = 0;
    if (!r || revision < r->revision)
        return -NLP_STALE;
    for (unsigned i = 0; i < NLP_ROUTES; ++i) {
        struct nlp_route *p = &r->routes[i];
        if (p->revision == revision && p->prepared) {
            p->prepared = 0;
            p->active = 1;
            found = 1;
        }
    }
    if (!found)
        return -NLP_NOT_FOUND;
    r->revision = revision;
    return 0;
}
void nlp_router_revoke(struct nlp_router *r, uint16_t node)
{
    for (unsigned i = 0; i < NLP_ROUTES; ++i)
        if (r->routes[i].node == node)
            memset(&r->routes[i], 0, sizeof(r->routes[i]));
}
void nlp_router_port_down(struct nlp_router *r, uint8_t port)
{
    if (r->parent == port) {
        r->epoch = 0;
        r->node = 0;
        r->parent = NLP_NO_PORT;
    }
    for (unsigned i = 0; i < NLP_ROUTES; ++i)
        if (!r->epoch || r->routes[i].port == port)
            memset(&r->routes[i], 0, sizeof(r->routes[i]));
}
void nlp_router_expire(struct nlp_router *r, uint32_t now)
{
    for (unsigned i = 0; i < NLP_ROUTES; ++i) {
        struct nlp_route *p = &r->routes[i];
        if ((p->active || p->prepared) && (int32_t)(now - p->expires_ms) >= 0)
            memset(p, 0, sizeof(*p));
    }
}
int nlp_router_forward(struct nlp_router *r, uint8_t ingress, uint8_t *raw, size_t size,
                       uint8_t *egress)
{
    struct nlp_frame f;
    struct nlp_route *src = NULL, *dst = NULL;
    int error;
    if (!r || !egress || (ingress >= NLP_PORTS && ingress != NLP_NO_PORT))
        return -NLP_INVALID;
    error = nlp_frame_inspect(raw, size, &f);
    if (error)
        return error;
    if (!(f.h.flags & NLP_ENCRYPTED) && nlp_frame_decode(raw, size, &f))
        return -NLP_INTEGRITY;
    if (!r->epoch || f.h.fabric_epoch != r->epoch || !f.h.session_id || f.h.src_node >= 65534 ||
        f.h.dst_node >= 65534)
        return -NLP_DENIED;
    for (unsigned i = 0; i < NLP_ROUTES; ++i)
        if (r->routes[i].active) {
            if (r->routes[i].node == f.h.src_node)
                src = &r->routes[i];
            if (r->routes[i].node == f.h.dst_node)
                dst = &r->routes[i];
        }
    if (ingress != NLP_NO_PORT && f.h.src_node == r->node)
        return -NLP_DENIED;
    if (ingress != NLP_NO_PORT && ingress != r->parent && (!src || src->port != ingress))
        return -NLP_DENIED;
    if (ingress == NLP_NO_PORT && f.h.src_node != r->node)
        return -NLP_DENIED;
    if (f.h.dst_node == r->node) {
        *egress = NLP_NO_PORT;
        return 0;
    }
    uint8_t port = dst ? dst->port : r->parent;
    if (port == NLP_NO_PORT || port == ingress)
        return -NLP_NO_ROUTE;
    if (ingress != NLP_NO_PORT) {
        if (f.h.hop_limit <= 1)
            return -NLP_TIMEOUT;
        raw[7]--;
    }
    nlp_put32(raw + 48, nlp_crc32c(raw, 48));
    if (!(f.h.flags & NLP_ENCRYPTED))
        nlp_put32(raw + size - 4, nlp_crc32c(raw, size - 4));
    *egress = port;
    return 0;
}
