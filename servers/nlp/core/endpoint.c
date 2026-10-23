/* Copyright (C) 2026 NOS contributors. */
#include "nlp/endpoint.h"
#include "nlp/auth.h"
#include "nlp/fabric.h"
#include <string.h>
static int dispatch(void *, struct nlp_session *, const struct nlp_frame *);
static int send_packet(void *context, const uint8_t *bytes, size_t size)
{
    struct nlp_binding *b = context;
    return b->endpoint->config.send(b->endpoint->config.context, bytes, size);
}
void nlp_endpoint_disconnect(struct nlp_endpoint *e)
{
    if (!e)
        return;
    for (unsigned i = 0; i < NLP_SESSIONS; ++i)
        nlp_session_close(&e->bindings[i].session, NLP_DISCONNECTED);
    memset(e->bindings, 0, sizeof(e->bindings));
    memset(&e->pool, 0, sizeof(e->pool));
    if (e->config.directory && e->peer_node)
        nlp_fabric_remove(e->config.directory, e->peer_node);
    if (e->joined && e->topology_revision < UINT32_MAX)
        ++e->topology_revision;
    nlp_auth_reset(e->config.auth);
    e->seen = e->joined = 0;
    e->peer_node = 0;
}
int nlp_endpoint_init(struct nlp_endpoint *e, const struct nlp_endpoint_config *config)
{
    if (!e || !config || !config->send || !config->boot_id || !config->epoch || !config->node ||
        config->node >= 65534)
        return -NLP_INVALID;
    memset(e, 0, sizeof(*e));
    e->config = *config;
    e->next_session = e->peer_next_session = e->topology_revision = 1;
    e->next_node = 2;
    e->revision = 1;
    return 0;
}
int nlp_endpoint_add(struct nlp_endpoint *e, const struct nlp_capability *c)
{
    if (!e || !c || !c->id || c->id == 65535 || !c->major || !c->call || !(c->roles & 3) ||
        (c->roles & ~3u))
        return -NLP_INVALID;
    if (e->count == NLP_CAPABILITIES || e->revision == UINT32_MAX)
        return -NLP_NO_SPACE;
    if (e->joined)
        return -NLP_BUSY; /* Freeze descriptors while handles are live. */
    for (unsigned i = 0; i < e->count; ++i)
        if (e->capabilities[i].id == c->id)
            return -NLP_CONFLICT;
    e->capabilities[e->count++] = *c;
    ++e->revision;
    return 0;
}
static void closed(void *context, struct nlp_session *session, unsigned reason)
{
    struct nlp_binding *b = context;
    (void)reason;
    if (b->capability && b->capability->disconnect)
        b->capability->disconnect(b->capability->context, session->id);
    b->capability = NULL;
}
static int binding_start(struct nlp_endpoint *e, struct nlp_binding *b, uint32_t id, uint16_t local,
                         uint16_t remote, unsigned reliable)
{
    for (unsigned i = 0; i < NLP_SESSIONS; ++i)
        if (e->bindings[i].session.active && e->bindings[i].session.id == id)
            return -NLP_CONFLICT;
    memset(b, 0, sizeof(*b));
    b->endpoint = e;
    nlp_session_init(&b->session, &e->pool);
    b->session.id = id;
    b->session.epoch = e->config.epoch;
    b->session.local_node = e->config.node;
    b->session.remote_node = e->peer_node;
    b->session.local_cap = local;
    b->session.remote_cap = remote;
    b->session.payload_limit = e->peer_payload;
    b->session.reliable_mask = (uint8_t)(1 | (reliable ? 2 : 0));
    b->session.send = send_packet;
    b->session.message = dispatch;
    b->session.context = b;
    b->session.closed = closed;
    b->permissions = e->config.permissions;
    return nlp_session_start(&b->session, e->now_ms);
}
int nlp_endpoint_associate(struct nlp_endpoint *e, uint16_t peer, uint32_t id,
                           const struct nlp_cipher *c0, const struct nlp_cipher *c1)
{
    int error;
    if (!e || !e->seen || !peer || peer == e->config.node || peer >= 65534 || !id || e->joined ||
        ((!c0 || !c1) && !e->config.physical))
        return -NLP_DENIED;
    if (id == UINT32_MAX)
        return -NLP_RANGE;
    if (memcmp(e->config.uid, e->peer_uid, 16) < 0) {
        if (id >= e->next_session)
            e->next_session = id + 1;
    } else if (id >= e->peer_next_session)
        e->peer_next_session = id + 1;
    if (peer >= e->next_node)
        e->next_node = (uint16_t)(peer + 1);
    e->peer_node = peer;
    error = binding_start(e, &e->bindings[0], id, 0, 0, 1);
    if (error)
        return error;
    e->bindings[0].session.channel[0].cipher = c0;
    e->bindings[0].session.channel[1].cipher = c1;
    e->joined = 1;
    if (e->topology_revision < UINT32_MAX)
        ++e->topology_revision;
    return 0;
}
static int control_reply(struct nlp_endpoint *e, const struct nlp_frame *f, unsigned opcode,
                         unsigned status, const uint8_t *body, size_t size)
{
    struct nlp_header h;
    uint8_t payload[NLP_PAYLOAD_MAX], raw[NLP_PDU_MAX];
    size_t written;
    if (size > NLP_PAYLOAD_MAX - 8)
        return -NLP_NO_SPACE;
    memset(&h, 0, sizeof(h));
    h.kind = NLP_CONTROL;
    h.priority = 0;
    h.hop_limit = 1;
    h.src_node = e->config.node;
    h.dst_node = f->h.src_node;
    h.opcode = (uint16_t)opcode;
    h.fabric_epoch = e->config.epoch;
    nlp_put32(payload, nlp_get32(f->payload));
    nlp_put16(payload + 4, (uint16_t)status);
    nlp_put16(payload + 6, 0);
    if (size)
        memcpy(payload + 8, body, size);
    if (opcode == NLP_OP_JOIN_ACCEPT && !status && e->config.auth &&
        e->config.auth->noise.complete) {
        if (size > NLP_PAYLOAD_MAX - 44)
            return -NLP_NO_SPACE;
        nlp_put16(payload + 8 + size, 22);
        nlp_put16(payload + 10 + size, 32);
        int error = nlp_auth_join_mac(e, &h, payload, size + 44, 1, payload + size + 12);
        if (error)
            return error;
        size += 36;
    }
    int error = nlp_frame_encode(&h, payload, size + 8, raw, sizeof(raw), &written);
    return error ? error : e->config.send(e->config.context, raw, written);
}
static int hello(struct nlp_endpoint *e, const struct nlp_frame *f)
{
    struct nlp_tlv_reader reader = {f->payload + 8, f->length - 8u, 0, 0, 0};
    struct nlp_tlv t;
    uint8_t body[NLP_PAYLOAD_MAX - 8], temp[16], uid[16];
    uint64_t boot = 0;
    uint32_t nonce = 0;
    uint16_t mask = 0, maximum = 0;
    size_t used = 0;
    int next;
    if (f->h.hop_limit != 1)
        return -NLP_DENIED;
    while ((next = nlp_tlv_next(&reader, &t)) > 0) {
        unsigned tag = t.tag & 0x7fff;
        if (tag <= 8)
            mask |= (uint16_t)(1u << tag);
        switch (tag) {
        case 1:
            if (t.length != 4)
                return -NLP_INVALID;
            nonce = nlp_get32(t.value);
            break;
        case 2:
            if (t.length != 16)
                return -NLP_INVALID;
            memcpy(uid, t.value, 16);
            break;
        case 3:
            if (t.length != 8)
                return -NLP_INVALID;
            boot = nlp_get64(t.value);
            break;
        case 4:
            if (t.length != 2 || t.value[0] > 1 || t.value[1] < 1)
                return -NLP_UNSUPPORTED;
            break;
        case 5:
            if (t.length != 2)
                return -NLP_INVALID;
            maximum = nlp_get16(t.value);
            break;
        case 6:
        case 7:
        case 8:
            if (t.length != 2)
                return -NLP_INVALID;
            break;
        default:
            if (t.tag & 0x8000)
                return -NLP_UNSUPPORTED;
            break;
        }
    }
    if (next < 0 || mask != 0x1fe || !boot || maximum < 128 || maximum > 1024 ||
        !memcmp(uid, e->config.uid, 16))
        return -NLP_INVALID;
    if (e->joined && (memcmp(uid, e->peer_uid, 16) || boot != e->peer_boot)) {
        if (!e->config.physical || e->bindings[0].session.channel[1].cipher)
            return -NLP_BUSY;
        nlp_endpoint_disconnect(e);
    }
    if (memcmp(uid, e->peer_uid, 16) || boot != e->peer_boot)
        e->peer_next_session = 1;
    memcpy(e->peer_uid, uid, 16);
    e->peer_boot = boot;
    e->peer_nonce = nonce;
    e->peer_payload = maximum < NLP_PAYLOAD_MAX ? maximum : NLP_PAYLOAD_MAX;
    e->seen = 1;
    e->hello_ms = e->now_ms;
#define ADD(tag, data, len)                                                                        \
    do {                                                                                           \
        int err = nlp_tlv_add(body, sizeof(body), &used, tag, data, len);                          \
        if (err)                                                                                   \
            return err;                                                                            \
    } while (0)
    nlp_put32(temp, nonce);
    ADD(1, temp, 4);
    ADD(2, e->config.uid, 16);
    nlp_put64(temp, e->config.boot_id);
    ADD(3, temp, 8);
    temp[0] = temp[1] = 1;
    ADD(4, temp, 2);
    nlp_put16(temp, NLP_PAYLOAD_MAX);
    ADD(5, temp, 2);
    nlp_put16(temp, e->config.node == 1 ? 6 : 3);
    ADD(6, temp, 2);
    nlp_put16(temp, (e->config.physical ? 1 : 0) | (e->config.auth ? 2 : 0));
    ADD(7, temp, 2);
    nlp_put16(temp, 1);
    ADD(8, temp, 2);
    ADD(9, e->config.fabric, 16);
    nlp_put32(temp, e->config.epoch);
    ADD(10, temp, 4);
#undef ADD
    return control_reply(e, f, NLP_OP_HELLO_ACK, 0, body, used);
}
static int bootstrap(struct nlp_endpoint *e, const struct nlp_frame *f)
{
    if (f->length < 8 || !nlp_get32(f->payload) || nlp_get16(f->payload + 4) ||
        nlp_get16(f->payload + 6))
        return -NLP_INVALID;
    if (f->h.opcode == NLP_OP_HELLO)
        return hello(e, f);
    if (!e->seen || e->now_ms - e->hello_ms >= 15000 || f->h.hop_limit != 1)
        return -NLP_DENIED;
    if (f->h.opcode == NLP_OP_AUTH_START || f->h.opcode == NLP_OP_AUTH_STEP ||
        f->h.opcode == NLP_OP_AUTH_FINISH)
        return e->config.auth ? nlp_auth_control(e, f)
                              : control_reply(e, f, f->h.opcode, NLP_UNSUPPORTED, NULL, 0);
    if (f->h.opcode == NLP_OP_JOIN_REQUEST) {
        if (e->config.node != 1 ||
            (!e->config.physical && (!e->config.auth || !e->config.auth->noise.complete)))
            return control_reply(e, f, NLP_OP_JOIN_ACCEPT, NLP_DENIED, NULL, 0);
        if (e->config.auth && e->config.auth->noise.complete) {
            int error = nlp_auth_join_verify(e, f);
            if (error)
                return error;
        }
        struct nlp_tlv_reader reader = {f->payload + 8, f->length - 8u, 0, 0, 0};
        struct nlp_tlv t;
        unsigned fields = 0;
        uint32_t proposed = 0;
        int next;
        while ((next = nlp_tlv_next(&reader, &t)) > 0) {
            switch (t.tag & 0x7fff) {
            case 9:
                if (t.length != 16 || memcmp(t.value, e->config.fabric, 16))
                    return -NLP_STALE;
                fields |= 1;
                break;
            case 10:
                if (t.length != 4 || nlp_get32(t.value) != e->config.epoch)
                    return -NLP_STALE;
                fields |= 2;
                break;
            case 13:
                if (t.length != 16 || memcmp(t.value, e->config.uid, 16))
                    return -NLP_CONFLICT;
                fields |= 4;
                break;
            case 17:
                if (t.length != 4)
                    return -NLP_INVALID;
                proposed = nlp_get32(t.value);
                break;
            default:
                if (t.tag & 0x8000)
                    return -NLP_UNSUPPORTED;
                break;
            }
        }
        if (next < 0 || fields != 7)
            return -NLP_INVALID;
        uint32_t id;
        if (e->joined) {
            id = e->bindings[0].session.id;
        } else {
            int secure = e->config.auth && e->config.auth->noise.complete;
            if (e->next_node >= 65534 || e->topology_revision == UINT32_MAX)
                return -NLP_NO_SPACE;
            if (secure)
                id = e->config.auth->session_id;
            else if (memcmp(e->config.uid, e->peer_uid, 16) < 0) {
                if (proposed || e->next_session == UINT32_MAX)
                    return -NLP_CONFLICT;
                id = e->next_session++;
            } else {
                if (!proposed || proposed < e->peer_next_session || proposed == UINT32_MAX)
                    return -NLP_CONFLICT;
                id = proposed;
                e->peer_next_session = id + 1;
            }
            uint16_t peer_node = e->next_node++;
            if (e->config.directory) {
                int error = nlp_fabric_admit(e->config.directory, e->config.node, 0, e->peer_uid,
                                             e->peer_boot, e->peer_payload, e->now_ms, 15000);
                if (error)
                    return error;
                for (unsigned i = 0; i < NLP_DEVICES; ++i)
                    if (e->config.directory->devices[i].online &&
                        !memcmp(e->config.directory->devices[i].uid, e->peer_uid, 16))
                        peer_node = e->config.directory->devices[i].node;
            }
            int error;
            if (secure) {
                e->peer_node = peer_node;
                error = binding_start(e, &e->bindings[0], id, 0, 0, 1);
                if (!error)
                    error = nlp_auth_channel(e, &e->bindings[0], 0);
                if (!error) {
                    e->joined = 1;
                    ++e->topology_revision;
                }
            } else
                error = nlp_endpoint_associate(e, peer_node, id, NULL, NULL);
            if (error) {
                nlp_session_close(&e->bindings[0].session, NLP_DENIED);
                if (e->config.directory)
                    nlp_fabric_remove(e->config.directory, peer_node);
                return error;
            }
        }
        uint8_t body[80], temp[8];
        size_t used = 0;
        nlp_tlv_add(body, sizeof(body), &used, 9, e->config.fabric, 16);
        nlp_put32(temp, e->config.epoch);
        nlp_tlv_add(body, sizeof(body), &used, 10, temp, 4);
        nlp_put16(temp, e->peer_node);
        nlp_tlv_add(body, sizeof(body), &used, 11, temp, 2);
        nlp_put32(temp, 15000);
        nlp_tlv_add(body, sizeof(body), &used, 12, temp, 4);
        nlp_put32(temp, id);
        nlp_tlv_add(body, sizeof(body), &used, 17, temp, 4);
        nlp_put32(temp, e->revision);
        nlp_tlv_add(body, sizeof(body), &used, 18, temp, 4);
        return control_reply(e, f, NLP_OP_JOIN_ACCEPT, 0, body, used);
    }
    return control_reply(e, f, f->h.opcode, NLP_UNSUPPORTED, NULL, 0);
}
static int directory(struct nlp_binding *b, unsigned opcode, const uint8_t *in, size_t size,
                     uint8_t *out, size_t *used)
{
    struct nlp_endpoint *e = b->endpoint;
    size_t capacity = *used;
    *used = 0;
    if (!(b->permissions & NLP_PERMISSION_DISCOVER))
        return -NLP_DENIED;
    if (opcode == NLP_OP_CLOSE) {
        if (size != 4)
            return -NLP_INVALID;
        for (unsigned i = 1; i < NLP_SESSIONS; ++i) {
            if (e->bindings[i].session.active && e->bindings[i].session.id == nlp_get32(in)) {
                nlp_session_close(&e->bindings[i].session, NLP_DISCONNECTED);
                *used = 0;
                return 0;
            }
        }
        return -NLP_STALE;
    }
    if (opcode == NLP_OP_GET_TOPOLOGY) {
        if (size != 12 || nlp_get16(in + 10))
            return -NLP_INVALID;
        if (e->config.directory) {
            *used = capacity;
            return nlp_fabric_page(e->config.directory, nlp_get32(in), nlp_get32(in + 4),
                                   nlp_get16(in + 8), out, used);
        }
        uint32_t revision = nlp_get32(in), cursor = nlp_get32(in + 4);
        unsigned maximum = nlp_get16(in + 8);
        unsigned total = e->joined ? 2 : 1, count = 0;
        if (revision && revision != e->topology_revision)
            return -NLP_STALE;
        if ((!revision && cursor) || cursor > total || !maximum || capacity < 12)
            return -NLP_INVALID;
        *used = 12;
        while (cursor < total && count < maximum && capacity - *used >= 40) {
            uint8_t *p = out + *used;
            memset(p, 0, 40);
            memcpy(p, cursor ? e->peer_uid : e->config.uid, 16);
            nlp_put64(p + 16, cursor ? e->peer_boot : e->config.boot_id);
            nlp_put16(p + 24, cursor ? e->peer_node : e->config.node);
            nlp_put16(p + 26, cursor ? e->config.node : 0);
            nlp_put16(p + 28, e->peer_payload);
            nlp_put16(p + 30, cursor ? 0 : e->count);
            nlp_put32(p + 32, cursor ? 0 : e->revision);
            nlp_put32(p + 36, cursor ? 15000 : 0);
            *used += 40;
            ++cursor;
            ++count;
        }
        if (!count && cursor < total)
            return -NLP_MTU_TOO_SMALL;
        nlp_put32(out, e->topology_revision);
        nlp_put32(out + 4, cursor < total ? cursor : 0);
        nlp_put16(out + 8, (uint16_t)count);
        nlp_put16(out + 10, cursor < total ? 1 : 0);
        return 0;
    }
    if (opcode == NLP_OP_GET_DEVICE) {
        if (size || capacity < 32)
            return -NLP_INVALID;
        nlp_put32(out, e->revision);
        memcpy(out + 4, e->config.uid, 16);
        nlp_put64(out + 20, e->config.boot_id);
        nlp_put16(out + 28, e->count);
        nlp_put16(out + 30, 1);
        *used = 32;
        return 0;
    }
    if (opcode == NLP_OP_LIST_CAPS || opcode == NLP_OP_GET_PORTS) {
        if (size != 12 || nlp_get16(in + 10) || capacity < 12)
            return -NLP_INVALID;
        uint32_t revision = nlp_get32(in), cursor = nlp_get32(in + 4);
        unsigned limit = nlp_get16(in + 8), count = 0,
                 total = opcode == NLP_OP_LIST_CAPS ? e->count : 1;
        unsigned entry = opcode == NLP_OP_LIST_CAPS ? 28 : 16;
        if (revision && revision != e->revision)
            return -NLP_STALE;
        if ((!revision && cursor) || cursor > total || !limit)
            return -NLP_INVALID;
        *used = 12;
        while (cursor < total && count < limit && entry <= capacity - *used) {
            uint8_t *p = out + *used;
            memset(p, 0, entry);
            if (opcode == NLP_OP_LIST_CAPS) {
                const struct nlp_capability *c = &e->capabilities[cursor];
                nlp_put16(p, c->id);
                nlp_put16(p + 2, c->roles);
                memcpy(p + 4, c->type, 16);
                nlp_put16(p + 20, c->major);
                nlp_put16(p + 22, c->minor);
                nlp_put32(p + 24, c->revision);
            } else {
                nlp_put16(p, 0);
                p[2] = e->config.transport ? e->config.transport : 1;
                p[3] = e->joined ? 4 : 2;
                nlp_put16(p + 4, 6);
                nlp_put16(p + 6, e->peer_payload);
                nlp_put16(p + 8, e->peer_node);
                nlp_put32(p + 12, 1);
            }
            *used += entry;
            ++count;
            ++cursor;
        }
        if (!count && cursor < total)
            return -NLP_MTU_TOO_SMALL;
        nlp_put32(out, e->revision);
        nlp_put32(out + 4, cursor < total ? cursor : 0);
        nlp_put16(out + 8, (uint16_t)count);
        nlp_put16(out + 10, cursor < total ? 1 : 0);
        return 0;
    }
    if (opcode == NLP_OP_GET_CAP_DESC) {
        if (size != 12 || nlp_get16(in + 6) || capacity < 12)
            return -NLP_INVALID;
        if (nlp_get32(in) && nlp_get32(in) != e->revision)
            return -NLP_STALE;
        const struct nlp_capability *c = NULL;
        for (unsigned i = 0; i < e->count; ++i)
            if (e->capabilities[i].id == nlp_get16(in + 4))
                c = &e->capabilities[i];
        if (!c)
            return -NLP_NOT_FOUND;
        uint32_t cursor = nlp_get32(in + 8);
        if (cursor > 8 || (!nlp_get32(in) && cursor))
            return -NLP_INVALID;
        uint8_t values[8][16] = {{0}};
        unsigned sizes[8] = {1, 1, 16, 8, 4, 2, 1, 1}, count = 0;
        values[0][0] = c->roles;
        memcpy(values[2], e->config.uid, 16);
        nlp_put32(values[4], c->permissions);
        nlp_put16(values[5], e->peer_payload);
        values[6][0] = 2;
        values[7][0] = 1;
        *used = 12;
        while (cursor < 8 && *used + 4 + sizes[cursor] <= capacity) {
            nlp_tlv_add(out, capacity, used, (uint16_t)(cursor + 1), values[cursor], sizes[cursor]);
            ++cursor;
            ++count;
        }
        nlp_put32(out, e->revision);
        nlp_put32(out + 4, cursor < 8 ? cursor : 0);
        nlp_put16(out + 8, (uint16_t)count);
        nlp_put16(out + 10, cursor < 8 ? 1 : 0);
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
static int open_cap(struct nlp_binding *b, const uint8_t *in, size_t size, uint8_t *out,
                    size_t *used)
{
    struct nlp_endpoint *e = b->endpoint;
    const struct nlp_capability *cap = NULL;
    struct nlp_binding *target = NULL;
    uint32_t id, permissions;
    if (size != 52 || *used < 24 || in[45] || in[46] || in[47] || in[42] > 1 || !in[43] ||
        in[43] > 2 || in[44] != 1)
        return -NLP_INVALID;
    for (unsigned i = 0; i < e->count; ++i)
        if (e->capabilities[i].id == nlp_get16(in + 4))
            cap = &e->capabilities[i];
    if (!cap)
        return -NLP_NOT_FOUND;
    if (memcmp(in + 8, cap->type, 16) || nlp_get16(in + 24) != cap->major ||
        nlp_get16(in + 26) > cap->minor || nlp_get64(in + 28))
        return -NLP_UNSUPPORTED;
    permissions = nlp_get32(in + 36);
    if ((permissions & ~b->permissions) || (cap->permissions & ~permissions))
        return -NLP_DENIED;
    /* Secure associations must supply per-capability derived keys; no silent plaintext downgrade.
     */
    if (b->session.channel[1].cipher && (!e->config.auth || !e->config.auth->noise.complete))
        return -NLP_DENIED;
    for (unsigned i = 1; i < NLP_SESSIONS; ++i)
        if (!e->bindings[i].session.active) {
            target = &e->bindings[i];
            break;
        }
    if (!target)
        return -NLP_NO_SPACE;
    id = nlp_get32(in);
    if (memcmp(e->config.uid, e->peer_uid, 16) < 0) {
        if (id || e->next_session == UINT32_MAX)
            return -NLP_CONFLICT;
        id = e->next_session++;
    } else {
        if (!id || id < e->peer_next_session || id == UINT32_MAX)
            return -NLP_CONFLICT;
        e->peer_next_session = id + 1;
    }
    uint16_t payload = nlp_get16(in + 40);
    uint32_t lease = nlp_get32(in + 48);
    if (payload < 64 || payload > e->peer_payload || !lease || lease > 15000)
        return -NLP_INVALID;
    int error = binding_start(e, target, id, cap->id, nlp_get16(in + 6), in[42]);
    if (error)
        return error;
    target->capability = cap;
    if (e->config.auth && e->config.auth->noise.complete) {
        error = nlp_auth_channel(e, target, (unsigned)(target - e->bindings));
        if (error) {
            nlp_session_close(&target->session, NLP_DENIED);
            return error;
        }
    }
    target->permissions = permissions;
    target->session.lease_ms = lease;
    target->session.payload_limit = payload;
    memset(out, 0, 24);
    nlp_put32(out, id);
    nlp_put16(out + 12, payload);
    out[14] = in[42];
    out[15] = in[43];
    out[16] = 1;
    nlp_put32(out + 20, lease);
    *used = 24;
    return 0;
}
static int dispatch(void *context, struct nlp_session *session, const struct nlp_frame *f)
{
    struct nlp_binding *b = context;
    struct nlp_endpoint *e = b->endpoint;
    if (!b->capability && f->h.kind == NLP_CONTROL &&
        (f->h.opcode == NLP_OP_ROUTE_INSTALL || f->h.opcode == NLP_OP_ROUTE_RESULT))
        return nlp_route_dispatch(b, f);
    if (f->h.kind == NLP_CLOSE) {
        if (f->h.opcode != NLP_OP_CLOSE || f->length != 8)
            return -NLP_INVALID;
        nlp_session_close(session, NLP_DISCONNECTED);
        return 0;
    }
    if (f->h.kind == NLP_CONTROL && f->h.opcode == NLP_OP_PING && f->length == 8)
        return nlp_session_send(session, f->h.channel_id, NLP_CONTROL, NLP_OP_PONG, 0, f->payload,
                                8, e->now_ms);
    if (b->capability &&
        ((f->h.kind == NLP_DATA && f->h.opcode >= 0x2000 && f->h.opcode < 0x3000) ||
         (f->h.kind == NLP_EVENT && f->h.opcode >= 0x1000 && f->h.opcode < 0x2000))) {
        size_t used = 0;
        return b->capability->call(b->capability->context, b, f->h.opcode, f->payload, f->length,
                                   NULL, &used);
    }
    if (f->h.kind != NLP_REQUEST || f->length < 8 || nlp_get16(f->payload + 4) || f->payload[6] ||
        f->payload[7])
        return -NLP_INVALID;
    uint32_t request = nlp_get32(f->payload);
    if (!request)
        return -NLP_INVALID;
    if (request < b->request_id) {
        uint8_t response[8] = {0};
        nlp_put32(response, request);
        nlp_put16(response + 4, NLP_OUTCOME_UNKNOWN);
        return nlp_session_send(session, f->h.channel_id, NLP_RESPONSE, f->h.opcode, 0, response, 8,
                                e->now_ms);
    }
    if (request == b->request_id) {
        if (f->h.opcode != b->request_opcode || f->length != b->request_size ||
            memcmp(f->payload, b->request, f->length))
            return -NLP_CONFLICT;
    } else {
        size_t used = session->payload_limit - 8;
        int result;
        if (request != b->request_id + 1)
            return -NLP_RANGE;
        if (!b->capability) {
            if (f->h.opcode == NLP_OP_OPEN)
                result = open_cap(b, f->payload + 8, f->length - 8, b->response + 8, &used);
            else
                result = directory(b, f->h.opcode, f->payload + 8, f->length - 8, b->response + 8,
                                   &used);
        } else
            result = b->capability->call(b->capability->context, b, f->h.opcode, f->payload + 8,
                                         f->length - 8, b->response + 8, &used);
        if (used > session->payload_limit - 8u || result > 0 || result < -NLP_MTU_TOO_SMALL)
            return -NLP_INVALID;
        if (result)
            used = 0;
        nlp_put32(b->response, request);
        nlp_put16(b->response + 4, (uint16_t)-result);
        b->response[6] = b->response[7] = 0;
        b->response_size = (uint16_t)(used + 8);
        b->request_id = request;
        b->request_size = f->length;
        b->request_opcode = f->h.opcode;
        memcpy(b->request, f->payload, f->length);
    }
    return nlp_session_send(session, f->h.channel_id, NLP_RESPONSE, f->h.opcode, 0, b->response,
                            b->response_size, e->now_ms);
}
int nlp_binding_event(struct nlp_binding *b, unsigned opcode, const void *bytes, size_t size)
{
    if (!b || !b->endpoint || !b->capability)
        return -NLP_INVALID;
    return nlp_session_send(&b->session, 1, NLP_EVENT, opcode, 1, bytes, size, b->endpoint->now_ms);
}
int nlp_endpoint_receive(void *context, const uint8_t *raw, size_t size)
{
    struct nlp_endpoint *e = context;
    struct nlp_frame f;
    int error;
    if (!e)
        return -NLP_INVALID;
    error = nlp_frame_inspect(raw, size, &f);
    if (error) {
        ++e->bad_frames;
        return error;
    }
    if (!f.h.session_id) {
        error = nlp_frame_decode(raw, size, &f);
        return error ? error : bootstrap(e, &f);
    }
    if (!e->joined)
        return -NLP_DENIED;
    for (unsigned i = 0; i < NLP_SESSIONS; ++i) {
        struct nlp_session *s = &e->bindings[i].session;
        if (s->active && s->id == f.h.session_id)
            return nlp_session_receive(s, raw, size, e->now_ms);
    }
    return -NLP_STALE;
}
void nlp_endpoint_poll(struct nlp_endpoint *e, uint32_t now)
{
    if (!e)
        return;
    if (e->clock_valid)
        e->monotonic_ms += (uint32_t)(now - e->now_ms);
    e->clock_valid = 1;
    e->now_ms = now;
    if (e->config.directory)
        nlp_fabric_poll(e->config.directory, now);
    if (e->seen && !e->joined && now - e->hello_ms >= 15000) {
        nlp_auth_reset(e->config.auth);
        e->seen = 0;
    }
    for (unsigned i = 0; i < NLP_SESSIONS; ++i)
        nlp_session_poll(&e->bindings[i].session, now);
    if (e->joined && !e->bindings[0].session.active)
        nlp_endpoint_disconnect(e);
}
