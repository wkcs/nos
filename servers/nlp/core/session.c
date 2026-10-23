/* Copyright (C) 2026 NOS contributors. */
#include "nlp/session.h"
#include <string.h>
#define SLOT_TX 1u
#define SLOT_RX_EMPTY 2u
#define SLOT_RX_DATA 3u
static struct nlp_slot *reserve(struct nlp_session *s, unsigned channel, unsigned role)
{
    /* Two physical slots reserved exclusively for management/control. */
    unsigned first = channel ? 2 : 0, end = channel ? NLP_POOL_SLOTS : 2;
    unsigned free_count = 0;
    for (unsigned i = first; i < end; ++i)
        if (!s->pool->slot[i].owner)
            ++free_count;
    if (role == SLOT_RX_EMPTY && free_count <= 1)
        return NULL;
    for (unsigned i = first; i < end; ++i) {
        struct nlp_slot *p = &s->pool->slot[i];
        if (!p->owner) {
            memset(p, 0, sizeof(*p));
            p->owner = s;
            p->channel = (uint8_t)channel;
            p->role = (uint8_t)role;
            return p;
        }
    }
    return NULL;
}
void nlp_session_init(struct nlp_session *s, struct nlp_pool *pool)
{
    memset(s, 0, sizeof(*s));
    s->pool = pool;
    s->payload_limit = NLP_PAYLOAD_MAX;
    s->window = 1;
    s->reliable_mask = 3;
    s->rto_ms = 100;
    s->lease_ms = 15000;
}
int nlp_session_start(struct nlp_session *s, uint32_t now)
{
    if (!s || !s->pool || !s->send || !s->message || !s->id || !s->epoch || !s->local_node ||
        !s->remote_node || s->local_node >= 65534 || s->remote_node >= 65534 || !s->window ||
        s->window > NLP_WINDOW || s->payload_limit > NLP_PAYLOAD_MAX || s->payload_limit < 64 ||
        !s->lease_ms || s->lease_ms > INT32_MAX || s->rto_ms < 20 || s->rto_ms > 2000 || s->active)
        return -NLP_INVALID;
    s->active = 1;
    s->last_receive_ms = now;
    for (unsigned i = 0; i < NLP_CHANNELS; ++i)
        s->channel[i].counter = 1;
    return 0;
}
static int encode(struct nlp_session *, unsigned, unsigned, unsigned, unsigned, uint32_t,
                  const void *, size_t, uint8_t *, size_t *);
void nlp_session_close(struct nlp_session *s, unsigned reason)
{
    if (!s || !s->active)
        return;
    s->active = 0;
    if (s->send && s->channel[0].counter && s->channel[0].counter < UINT32_MAX) {
        uint8_t payload[8], raw[NLP_PDU_MAX];
        size_t size;
        nlp_put32(payload, s->channel[0].counter);
        nlp_put16(payload + 4, (uint16_t)reason);
        nlp_put16(payload + 6, 0);
        if (!encode(s, 0, NLP_CLOSE, NLP_OP_CLOSE, 0, 0, payload, 8, raw, &size))
            (void)s->send(s->context, raw, size);
    }
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i)
        if (s->pool->slot[i].owner == s)
            memset(&s->pool->slot[i], 0, sizeof(s->pool->slot[i]));
    if (s->closed)
        s->closed(s->context, s, reason);
}
static int encode(struct nlp_session *s, unsigned ch, unsigned kind, unsigned opcode,
                  unsigned priority, uint32_t seq, const void *data, size_t length, uint8_t *out,
                  size_t *size)
{
    struct nlp_channel *c = &s->channel[ch];
    struct nlp_header h;
    if (c->counter == UINT32_MAX) {
        nlp_session_close(s, NLP_RANGE);
        return -NLP_RANGE;
    }
    memset(&h, 0, sizeof(h));
    h.kind = (uint8_t)kind;
    h.priority = (uint8_t)priority;
    h.hop_limit = 16;
    h.fabric_epoch = s->epoch;
    h.session_id = s->id;
    h.src_node = s->local_node;
    h.dst_node = s->remote_node;
    h.src_cap = s->local_cap;
    h.dst_cap = s->remote_cap;
    h.channel_id = (uint16_t)ch;
    h.opcode = (uint16_t)opcode;
    h.packet_counter = c->counter++;
    h.reliable_seq = seq;
    if (seq)
        h.flags = NLP_RELIABLE;
    if (seq || kind == NLP_ACK) {
        h.ack = c->rx_ack;
        h.receive_limit = c->rx_limit;
    }
    if (c->cipher) {
        h.flags |= NLP_ENCRYPTED;
        return nlp_cipher_encode(c->cipher, &h, data, length, out, NLP_PDU_MAX, size);
    }
    return nlp_frame_encode(&h, data, length, out, NLP_PDU_MAX, size);
}
int nlp_session_send(struct nlp_session *s, unsigned ch, unsigned kind, unsigned opcode,
                     unsigned priority, const void *data, size_t length, uint32_t now)
{
    struct nlp_slot *slot;
    uint8_t raw[NLP_PDU_MAX];
    size_t size;
    int error;
    if (!s || !s->active)
        return -NLP_DISCONNECTED;
    if (ch >= NLP_CHANNELS || length > s->payload_limit || kind > NLP_CLOSE || kind == NLP_ACK ||
        priority > 3 || opcode > 65535)
        return -NLP_INVALID;
    if (!(s->reliable_mask & (1u << ch))) {
        error = encode(s, ch, kind, opcode, priority, 0, data, length, raw, &size);
        return error ? error : s->send(s->context, raw, size);
    }
    struct nlp_channel *c = &s->channel[ch];
    if (c->tx_seq == UINT32_MAX) {
        nlp_session_close(s, NLP_RANGE);
        return -NLP_RANGE;
    }
    if (c->tx_seq >= c->tx_limit)
        return -NLP_BUSY;
    slot = reserve(s, ch, SLOT_TX);
    if (!slot)
        return -NLP_BUSY;
    error = encode(s, ch, kind, opcode, priority, c->tx_seq + 1, data, length, slot->bytes, &size);
    if (error) {
        memset(slot, 0, sizeof(*slot));
        return error;
    }
    slot->header.reliable_seq = ++c->tx_seq;
    slot->length = (uint16_t)size;
    slot->due_ms = now;
    return 0; /* Accepted into bounded queue; poll owns physical send/retry. */
}
static struct nlp_slot *rx_sequence(struct nlp_session *s, unsigned ch, uint32_t seq)
{
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i) {
        struct nlp_slot *p = &s->pool->slot[i];
        if (p->owner == s && p->role == SLOT_RX_DATA && p->channel == ch &&
            p->header.reliable_seq == seq)
            return p;
    }
    return NULL;
}
static int apply_ack(struct nlp_session *s, unsigned ch, uint32_t ack, uint32_t limit)
{
    struct nlp_channel *c = &s->channel[ch];
    if (ack > c->tx_seq || limit < ack || limit - ack > s->window)
        return -NLP_INVALID;
    if (ack > c->tx_acked)
        c->tx_acked = ack;
    if (limit > c->tx_limit)
        c->tx_limit = limit;
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i) {
        struct nlp_slot *p = &s->pool->slot[i];
        if (p->owner == s && p->channel == ch && p->role == SLOT_TX &&
            p->header.reliable_seq <= c->tx_acked)
            memset(p, 0, sizeof(*p));
    }
    return 0;
}
static int replay(struct nlp_channel *c, uint32_t count)
{
    if (count > c->replay_top) {
        uint32_t delta = count - c->replay_top;
        c->replay_bits = delta >= 64 ? 1 : (c->replay_bits << delta) | 1;
        c->replay_top = count;
    } else {
        uint32_t delta = c->replay_top - count;
        if (delta >= 64 || (c->replay_bits & (UINT64_C(1) << delta)))
            return -NLP_STALE;
        c->replay_bits |= UINT64_C(1) << delta;
    }
    return 0;
}
int nlp_session_receive(struct nlp_session *s, const uint8_t *raw, size_t size, uint32_t now)
{
    struct nlp_frame f;
    uint8_t plain[NLP_PAYLOAD_MAX];
    struct nlp_channel *c;
    struct nlp_slot *slot = NULL;
    int error;
    if (!s || !s->active)
        return -NLP_DISCONNECTED;
    error = nlp_frame_inspect(raw, size, &f);
    if (error)
        return error;
    if (f.h.channel_id >= NLP_CHANNELS || f.h.session_id != s->id || f.h.fabric_epoch != s->epoch ||
        f.h.src_node != s->remote_node || f.h.dst_node != s->local_node ||
        f.h.src_cap != s->remote_cap || f.h.dst_cap != s->local_cap || f.length > s->payload_limit)
        return -NLP_DENIED;
    unsigned ch = f.h.channel_id;
    c = &s->channel[ch];
    error = c->cipher ? nlp_cipher_decode(c->cipher, raw, size, &f, plain)
                      : nlp_frame_decode(raw, size, &f);
    if (error)
        return error;
    if (f.h.kind == NLP_CLOSE && !(f.h.flags & NLP_RELIABLE)) {
        if (f.h.channel_id || f.h.opcode != NLP_OP_CLOSE || f.length != 8 ||
            !nlp_get32(f.payload) || nlp_get16(f.payload + 4) > NLP_MTU_TOO_SMALL ||
            nlp_get16(f.payload + 6))
            return -NLP_INVALID;
        error = replay(c, f.h.packet_counter);
        if (!error)
            nlp_session_close(s, nlp_get16(f.payload + 4));
        return error;
    }
    if (f.h.kind == NLP_ACK || (f.h.flags & NLP_RELIABLE)) {
        if (!(s->reliable_mask & (1u << ch)))
            return -NLP_INVALID;
        error = apply_ack(s, ch, f.h.ack, f.h.receive_limit);
        if (error)
            return error;
    }
    if (f.h.kind == NLP_ACK) {
        error = replay(c, f.h.packet_counter);
        if (!error)
            s->last_receive_ms = now;
        return error;
    }
    if (!(s->reliable_mask & (1u << ch))) {
        if (f.h.flags & NLP_RELIABLE)
            return -NLP_INVALID;
        error = replay(c, f.h.packet_counter);
        if (error)
            return error;
        s->last_receive_ms = now;
        return s->message(s->context, s, &f);
    }
    if (!(f.h.flags & NLP_RELIABLE))
        return -NLP_INVALID;
    c->ack_pending = 1;
    if (f.h.reliable_seq <= c->rx_ack)
        return 0;
    if (f.h.reliable_seq > c->rx_limit)
        return -NLP_NO_SPACE;
    slot = rx_sequence(s, ch, f.h.reliable_seq);
    if (slot) {
        if (slot->length != f.length || slot->header.opcode != f.h.opcode ||
            memcmp(slot->bytes, f.payload, f.length))
            return -NLP_CONFLICT;
        return 0;
    }
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i) {
        struct nlp_slot *p = &s->pool->slot[i];
        if (p->owner == s && p->channel == ch && p->role == SLOT_RX_EMPTY) {
            slot = p;
            break;
        }
    }
    if (!slot)
        return -NLP_NO_SPACE;
    s->last_receive_ms = now;
    slot->role = SLOT_RX_DATA;
    slot->header = f.h;
    slot->length = f.length;
    memcpy(slot->bytes, f.payload, f.length);
    while (c->rx_ack < c->rx_limit && rx_sequence(s, ch, c->rx_ack + 1))
        ++c->rx_ack;
    return 0;
}
void nlp_session_poll(struct nlp_session *s, uint32_t now)
{
    if (!s || !s->active)
        return;
    if (now - s->last_receive_ms >= s->lease_ms) {
        nlp_session_close(s, NLP_TIMEOUT);
        return;
    }
    for (unsigned ch = 0; ch < NLP_CHANNELS && s->active; ++ch) {
        struct nlp_channel *c = &s->channel[ch];
        if (c->rx_delivered < c->rx_ack) {
            struct nlp_slot *p = rx_sequence(s, ch, c->rx_delivered + 1);
            if (p) {
                struct nlp_frame f = {p->header, p->bytes, p->length};
                int error = s->message(s->context, s, &f);
                if (!s->active)
                    return;
                if (error != -NLP_BUSY && error != -NLP_NO_SPACE) {
                    if (error) {
                        nlp_session_close(s, (unsigned)-error);
                        return;
                    }
                    memset(p, 0, sizeof(*p));
                    ++c->rx_delivered;
                }
            }
        }
        if (s->reliable_mask & (1u << ch)) {
            while (c->rx_limit - c->rx_delivered < s->window && c->rx_limit < UINT32_MAX) {
                if (!reserve(s, ch, SLOT_RX_EMPTY))
                    break;
                ++c->rx_limit;
                c->ack_pending = 1;
            }
            if (c->rx_limit && now - c->last_ack_ms >= 1000)
                c->ack_pending = 1;
            if (c->ack_pending) {
                uint8_t ack[NLP_PDU_MAX];
                size_t size;
                int error = encode(s, ch, NLP_ACK, 0, 0, 0, NULL, 0, ack, &size);
                if (!error && !s->send(s->context, ack, size)) {
                    c->ack_pending = 0;
                    c->last_ack_ms = now;
                }
            }
        }
    }
    for (unsigned i = 0; i < NLP_POOL_SLOTS && s->active; ++i) {
        struct nlp_slot *p = &s->pool->slot[i];
        if (p->owner != s || p->role != SLOT_TX || (int32_t)(now - p->due_ms) < 0)
            continue;
        if (p->retries >= 5) {
            nlp_session_close(s, NLP_TIMEOUT);
            return;
        }
        int error = s->send(s->context, p->bytes, p->length);
        if (!error) {
            if (p->sent)
                ++p->retries;
            p->sent = 1;
            uint32_t delay = s->rto_ms << p->retries;
            p->due_ms = now + (delay > 2000 ? 2000 : delay);
        } else if (error != -NLP_BUSY && error != -NLP_NO_SPACE) {
            nlp_session_close(s, NLP_DISCONNECTED);
            return;
        } else
            p->due_ms = now + 1;
    }
}
