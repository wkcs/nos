/* Copyright (C) 2026 NOS contributors. Two-phase tree admission; no address reuse in an epoch. */
#include "nlp/fabric.h"
#include <string.h>
static struct nlp_device_record *device(struct nlp_fabric *f, uint16_t node)
{
    for (unsigned i = 0; i < NLP_DEVICES; ++i)
        if (f->devices[i].present && f->devices[i].node == node)
            return &f->devices[i];
    return NULL;
}
static int changed(struct nlp_fabric *f)
{
    if (f->revision == UINT32_MAX)
        return -NLP_RANGE;
    ++f->revision;
    return 0;
}
int nlp_fabric_init(struct nlp_fabric *f, const uint8_t uid[16], uint64_t boot,
                    const uint8_t fabric[16], uint32_t epoch, const struct nlp_fabric_ops *ops)
{
    if (!f || !uid || !boot || !fabric || !epoch || !ops || !ops->control || !ops->joined ||
        !ops->gone)
        return -NLP_INVALID;
    memset(f, 0, sizeof(*f));
    f->ops = *ops;
    f->revision = f->next_revision = 1;
    f->next_node = 2;
    nlp_router_init(&f->router, uid);
    int error = nlp_router_join(&f->router, fabric, epoch, 1, NLP_NO_PORT, NULL, 0);
    if (error)
        return error;
    struct nlp_device_record *root = &f->devices[0];
    memcpy(root->uid, uid, 16);
    root->boot = boot;
    root->node = 1;
    root->payload_limit = NLP_PAYLOAD_MAX;
    root->present = root->online = 1;
    return 0;
}
static void abort_admission(struct nlp_fabric *, unsigned);
static int descendant(struct nlp_fabric *f, struct nlp_device_record *d, uint16_t root)
{
    for (unsigned depth = 0; d && depth <= 8; ++depth) {
        if (d->node == root)
            return 1;
        if (d->node == 1)
            break;
        d = device(f, d->parent);
    }
    return 0;
}
void nlp_fabric_remove(struct nlp_fabric *f, uint16_t node)
{
    uint8_t remove[NLP_DEVICES] = {0};
    if (!f || node <= 1)
        return;
    if (f->pending.active && descendant(f, device(f, f->pending.node), node))
        abort_admission(f, NLP_DISCONNECTED);
    for (unsigned i = 1; i < NLP_DEVICES; ++i)
        if (f->devices[i].present && descendant(f, &f->devices[i], node))
            remove[i] = 1;
    for (unsigned depth = 8; depth; --depth)
        for (unsigned i = 1; i < NLP_DEVICES; ++i)
            if (remove[i] && f->devices[i].depth == depth) {
                struct nlp_device_record *d = &f->devices[i];
                uint32_t revision = f->next_revision < UINT32_MAX ? ++f->next_revision : UINT32_MAX;
                struct nlp_device_record *parent = device(f, d->parent);
                for (unsigned hop = 0; parent && parent->node != 1 && hop < 8; ++hop) {
                    (void)f->ops.control(f->ops.context, parent->node, revision, 3, d->node, d->uid,
                                         d->uid, 0);
                    parent = device(f, parent->parent);
                }
                nlp_router_revoke(&f->router, d->node);
                f->ops.gone(f->ops.context, d->uid, d->boot, d->node);
                memset(d, 0, sizeof(*d));
                changed(f);
            }
}
static void abort_admission(struct nlp_fabric *f, unsigned status)
{
    struct nlp_admission p = f->pending;
    struct nlp_device_record *d = device(f, p.node);
    if (!p.active || !d)
        return;
    struct nlp_device_record copy = *d;
    for (unsigned i = 1; i < p.count; ++i) {
        struct nlp_device_record *ancestor = device(f, p.ancestors[i]);
        if (ancestor)
            f->ops.control(f->ops.context, ancestor->node, p.revision, 3, p.node, d->uid, d->uid,
                           0);
    }
    memset(&f->pending, 0, sizeof(f->pending));
    nlp_fabric_remove(f, p.node);
    f->ops.joined(f->ops.context, &copy, status);
}
static int send_phase(struct nlp_fabric *f, unsigned action)
{
    struct nlp_admission *p = &f->pending;
    struct nlp_device_record *target = device(f, p->node);
    p->phase = (uint8_t)action;
    p->acknowledged = 1;
    for (unsigned i = 1; i < p->count; ++i) {
        struct nlp_device_record *next = i + 1 < p->count ? device(f, p->ancestors[i + 1]) : target;
        int error = f->ops.control(f->ops.context, p->ancestors[i], p->revision, action,
                                   target->node, target->uid, next->uid, p->lease_ms);
        if (error) {
            abort_admission(f, (unsigned)-error);
            return error;
        }
    }
    return 0;
}
static int commit(struct nlp_fabric *f, uint32_t now)
{
    struct nlp_device_record *d = device(f, f->pending.node);
    int error = nlp_router_commit(&f->router, f->pending.revision);
    if (error) {
        abort_admission(f, (unsigned)-error);
        return error;
    }
    d->online = 1;
    d->expires_ms = now + f->pending.lease_ms;
    for (unsigned i = 0; i < NLP_ROUTES; ++i)
        if (f->router.routes[i].node == d->node)
            f->router.routes[i].expires_ms = d->expires_ms;
    changed(f);
    memset(&f->pending, 0, sizeof(f->pending));
    f->ops.joined(f->ops.context, d, NLP_OK);
    return 0;
}
static int begin(struct nlp_fabric *f, struct nlp_device_record *d, uint32_t now, uint32_t lease)
{
    f->now_ms = now;
    struct nlp_admission *p = &f->pending;
    struct nlp_device_record *parent = device(f, d->parent);
    if (!parent || parent->depth >= 8 || lease < 1000 || lease > 60000 ||
        f->next_revision == UINT32_MAX)
        return -NLP_INVALID;
    memset(p, 0, sizeof(*p));
    p->node = d->node;
    p->revision = ++f->next_revision;
    p->lease_ms = lease;
    p->deadline_ms = now + (lease / 2 < 5000 ? lease / 2 : 5000);
    p->active = 1;
    p->count = parent->depth + 1;
    for (unsigned i = p->count; i; --i) {
        p->ancestors[i - 1] = parent->node;
        parent = device(f, parent->parent);
    }
    int error =
        nlp_router_prepare(&f->router, d->node, d->uid, d->root_port, p->revision, now, lease);
    if (error) {
        memset(p, 0, sizeof(*p));
        return error;
    }
    error = send_phase(f, 1);
    if (error)
        return error;
    if (p->count == 1)
        return commit(f, now);
    return 0;
}
int nlp_fabric_admit(struct nlp_fabric *f, uint16_t parent_node, uint8_t port,
                     const uint8_t uid[16], uint64_t boot, uint16_t payload, uint32_t now,
                     uint32_t lease)
{
    if (!f || !uid || !boot || port >= NLP_PORTS || payload < 128 || payload > NLP_PAYLOAD_MAX)
        return -NLP_INVALID;
    if (f->pending.active)
        return -NLP_BUSY;
    struct nlp_device_record *parent = device(f, parent_node), *slot = NULL;
    if (!parent || !parent->online || parent->depth >= 8)
        return -NLP_NOT_FOUND;
    for (unsigned i = 0; i < NLP_DEVICES; ++i) {
        struct nlp_device_record *d = &f->devices[i];
        if (d->present && !memcmp(d->uid, uid, 16))
            return -NLP_CONFLICT;
        if (!d->present)
            slot = d;
    }
    if (!slot || f->next_node >= 65534)
        return -NLP_NO_SPACE;
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->uid, uid, 16);
    slot->boot = boot;
    slot->node = f->next_node++;
    slot->parent = parent_node;
    slot->depth = parent->depth + 1;
    slot->root_port = parent_node == 1 ? port : parent->root_port;
    slot->payload_limit = payload < parent->payload_limit ? payload : parent->payload_limit;
    slot->present = 1;
    int error = begin(f, slot, now, lease);
    if (error && slot->present && !f->pending.active)
        memset(slot, 0, sizeof(*slot));
    return error;
}
int nlp_fabric_ack(struct nlp_fabric *f, uint16_t from, uint32_t revision, unsigned action,
                   unsigned status, uint32_t now)
{
    if (!f || !f->pending.active || revision != f->pending.revision || action != f->pending.phase)
        return -NLP_STALE;
    struct nlp_admission *p = &f->pending;
    unsigned bit = 0;
    for (unsigned i = 1; i < p->count; ++i)
        if (p->ancestors[i] == from)
            bit = 1u << i;
    if (!bit)
        return -NLP_DENIED;
    if (p->acknowledged & bit)
        return 0;
    if (status) {
        abort_admission(f, status);
        return -(int)status;
    }
    p->acknowledged |= (uint8_t)bit;
    if (p->acknowledged == (1u << p->count) - 1) {
        if (action == 1)
            return send_phase(f, 2);
        return commit(f, now);
    }
    return 0;
}
int nlp_fabric_renew(struct nlp_fabric *f, uint16_t node, uint64_t boot, uint32_t now,
                     uint32_t lease)
{
    if (!f || f->pending.active)
        return -NLP_BUSY;
    struct nlp_device_record *d = device(f, node);
    if (!d || node == 1 || d->boot != boot || !d->online)
        return -NLP_STALE;
    return begin(f, d, now, lease);
}
void nlp_fabric_poll(struct nlp_fabric *f, uint32_t now)
{
    if (!f)
        return;
    f->now_ms = now;
    if (f->pending.active && (int32_t)(now - f->pending.deadline_ms) >= 0)
        abort_admission(f, NLP_TIMEOUT);
    for (unsigned i = 1; i < NLP_DEVICES; ++i) {
        struct nlp_device_record *d = &f->devices[i];
        if (d->present && d->online && (int32_t)(now - d->expires_ms) >= 0) {
            if (f->pending.active && descendant(f, device(f, f->pending.node), d->node))
                abort_admission(f, NLP_DISCONNECTED);
            nlp_fabric_remove(f, d->node);
        }
    }
    nlp_router_expire(&f->router, now);
}
int nlp_fabric_page(struct nlp_fabric *f, uint32_t revision, uint32_t cursor, unsigned maximum,
                    uint8_t *out, size_t *size)
{
    if (!f || !out || !size || *size < 12 || !maximum || cursor > NLP_DEVICES ||
        (!revision && cursor))
        return -NLP_INVALID;
    if (revision && revision != f->revision)
        return -NLP_STALE;
    size_t capacity = *size, used = 12;
    unsigned count = 0;
    while (cursor < NLP_DEVICES) {
        struct nlp_device_record *d = &f->devices[cursor];
        if (!d->online) {
            ++cursor;
            continue;
        }
        if (count == maximum || capacity - used < 40)
            break;
        uint8_t *p = out + used;
        memcpy(p, d->uid, 16);
        nlp_put64(p + 16, d->boot);
        nlp_put16(p + 24, d->node);
        nlp_put16(p + 26, d->parent);
        nlp_put16(p + 28, d->payload_limit);
        nlp_put16(p + 30, d->capabilities);
        nlp_put32(p + 32, d->descriptor_revision);
        nlp_put32(p + 36, d->node == 1 || (int32_t)(d->expires_ms - f->now_ms) <= 0
                              ? 0
                              : d->expires_ms - f->now_ms);
        used += 40;
        ++cursor;
        ++count;
    }
    if (cursor < NLP_DEVICES && !count)
        return -NLP_MTU_TOO_SMALL;
    nlp_put32(out, f->revision);
    nlp_put32(out + 4, cursor < NLP_DEVICES ? cursor : 0);
    nlp_put16(out + 8, (uint16_t)count);
    nlp_put16(out + 10, cursor < NLP_DEVICES ? 1 : 0);
    *size = used;
    return 0;
}
