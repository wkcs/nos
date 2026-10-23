/* Copyright (C) 2026 NOS contributors. Actual coordinator + actual per-hop routers. */
#include "nlp/fabric.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct control {
    uint16_t receiver, destination;
    uint32_t revision, lease;
    uint8_t action, target[16];
};
struct fabric_test {
    struct nlp_fabric fabric;
    struct nlp_router routers[32];
    struct control controls[128];
    unsigned head, tail, joined, gone, failed;
};
static struct nlp_device_record *record(struct fabric_test *t, uint16_t node)
{
    for (unsigned i = 0; i < NLP_DEVICES; ++i)
        if (t->fabric.devices[i].present && t->fabric.devices[i].node == node)
            return &t->fabric.devices[i];
    return NULL;
}
static int control(void *context, uint16_t receiver, uint32_t revision, unsigned action,
                   uint16_t destination, const uint8_t target[16], const uint8_t child[16],
                   uint32_t lease)
{
    struct fabric_test *t = context;
    (void)child;
    if (t->tail == 128)
        return -NLP_BUSY;
    struct control *c = &t->controls[t->tail++];
    c->receiver = receiver;
    c->revision = revision;
    c->action = (uint8_t)action;
    c->destination = destination;
    memcpy(c->target, target, 16);
    c->lease = lease;
    return 0;
}
static void joined(void *context, const struct nlp_device_record *d, unsigned status)
{
    struct fabric_test *t = context;
    if (status) {
        ++t->failed;
        return;
    }
    ++t->joined;
    assert(d->node < 32);
    if (t->routers[d->node].epoch)
        return;
    uint8_t ancestors[8][16];
    struct nlp_device_record *parent = record(t, d->parent);
    for (unsigned i = d->depth; i; --i) {
        assert(parent);
        memcpy(ancestors[i - 1], parent->uid, 16);
        parent = record(t, parent->parent);
    }
    nlp_router_init(&t->routers[d->node], d->uid);
    assert(!nlp_router_join(&t->routers[d->node], t->fabric.router.fabric, t->fabric.router.epoch,
                            d->node, 1, &ancestors[0][0], d->depth));
}
static void gone(void *context, const uint8_t uid[16], uint64_t boot, uint16_t node)
{
    struct fabric_test *t = context;
    (void)uid;
    (void)boot;
    ++t->gone;
    assert(node < 32);
    memset(&t->routers[node], 0, sizeof(t->routers[node]));
}
static void process(struct fabric_test *t, uint32_t now)
{
    while (t->head < t->tail) {
        struct control c = t->controls[t->head++];
        struct nlp_router *r = &t->routers[c.receiver];
        int error = 0;
        if (c.action == 1)
            error = nlp_router_prepare(r, c.destination, c.target, 0, c.revision, now, c.lease);
        else if (c.action == 2) {
            error = nlp_router_commit(r, c.revision);
            if (!error)
                for (unsigned i = 0; i < NLP_ROUTES; ++i)
                    if (r->routes[i].node == c.destination)
                        r->routes[i].expires_ms = now + c.lease;
        } else
            nlp_router_revoke(r, c.destination);
        if (c.action != 3) {
            int result = nlp_fabric_ack(&t->fabric, c.receiver, c.revision, c.action,
                                        error ? (unsigned)-error : 0, now);
            assert(!result || result == -NLP_STALE);
        }
    }
    t->head = t->tail = 0;
}
void nlp_fabric_tests(void)
{
    struct fabric_test t;
    memset(&t, 0, sizeof(t));
    uint8_t uid[16] = {1}, fabric[16] = {7};
    struct nlp_fabric_ops ops = {&t, control, joined, gone};
    assert(!nlp_fabric_init(&t.fabric, uid, 1, fabric, 1, &ops));
    for (unsigned depth = 1; depth <= 8; ++depth) {
        uid[0] = (uint8_t)(depth + 1);
        assert(!nlp_fabric_admit(&t.fabric, (uint16_t)depth, 0, uid, 1, 128, 0, 15000));
        if (depth > 1) {
            assert(t.fabric.pending.active);
            assert(nlp_fabric_ack(&t.fabric, 999, t.fabric.pending.revision, 1, 0, 0) ==
                   -NLP_DENIED);
        }
        process(&t, 0);
        assert(!t.fabric.pending.active && record(&t, (uint16_t)(depth + 1))->online);
    }
    assert(t.joined == 8);
    uid[0] = 99;
    assert(nlp_fabric_admit(&t.fabric, 9, 0, uid, 1, 128, 0, 15000) == -NLP_NOT_FOUND);
    uid[0] = 2;
    assert(nlp_fabric_admit(&t.fabric, 1, 0, uid, 2, 128, 0, 15000) == -NLP_CONFLICT);
    uint8_t raw[NLP_PDU_MAX], egress;
    size_t size;
    struct nlp_header h = {.kind = NLP_DATA,
                           .priority = 1,
                           .hop_limit = 16,
                           .fabric_epoch = 1,
                           .src_node = 9,
                           .dst_node = 1,
                           .session_id = 42,
                           .packet_counter = 1};
    assert(!nlp_frame_encode(&h, "cascade", 7, raw, sizeof(raw), &size));
    assert(!nlp_router_forward(&t.routers[9], NLP_NO_PORT, raw, size, &egress) && egress == 1);
    for (unsigned node = 8; node > 1; --node)
        assert(!nlp_router_forward(&t.routers[node], 0, raw, size, &egress) && egress == 1);
    assert(!nlp_router_forward(&t.fabric.router, 0, raw, size, &egress) && egress == NLP_NO_PORT);
    struct nlp_frame decoded;
    assert(!nlp_frame_decode(raw, size, &decoded) && decoded.h.hop_limit == 9 &&
           !memcmp(decoded.payload, "cascade", 7));
    uint8_t page[128];
    size = sizeof(page);
    assert(!nlp_fabric_page(&t.fabric, 0, 0, 2, page, &size) && nlp_get16(page + 8) == 2 &&
           nlp_get16(page + 10) == 1);
    uint32_t revision = nlp_get32(page), cursor = nlp_get32(page + 4);
    assert(!nlp_fabric_renew(&t.fabric, 9, 1, 1000, 15000));
    process(&t, 1000);
    size = sizeof(page);
    assert(nlp_fabric_page(&t.fabric, revision, cursor, 2, page, &size) == -NLP_STALE);
    nlp_fabric_remove(&t.fabric, 3);
    assert(t.gone == 7 && record(&t, 2) && !record(&t, 3));
    uid[0] = 55;
    assert(!nlp_fabric_admit(&t.fabric, 2, 0, uid, 1, 128, 1000, 15000));
    assert(t.fabric.pending.active);
    nlp_fabric_poll(&t.fabric, 6001);
    assert(!t.fabric.pending.active && t.failed == 1);
    process(&t, 6001);
    uid[0] = 56;
    assert(!nlp_fabric_admit(&t.fabric, 2, 0, uid, 1, 128, 6001, 15000));
    assert(t.fabric.pending.active);
    nlp_fabric_remove(&t.fabric, 2);
    assert(!t.fabric.pending.active && t.failed == 2 && !record(&t, 2));
    process(&t, 6001);
    uid[0] = 57;
    assert(!nlp_fabric_admit(&t.fabric, 1, 1, uid, 1, 128, 7000, 15000));
    assert(record(&t, 12) && record(&t, 12)->online);
    nlp_fabric_poll(&t.fabric, 22001);
    assert(!record(&t, 12));
    size = sizeof(page);
    assert(!nlp_fabric_page(&t.fabric, 0, 0, 8, page, &size) && nlp_get16(page + 8) == 1);
    puts("PASS "
         "nlp_fabric_eight_level_real_router_admission_commit_directory_stale_lease_parent_loss_"
         "and_no_address_reuse");
}
