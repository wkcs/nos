/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_SESSION_H
#define NOS_NLP_SESSION_H
#include "security.h"
#include "transport.h"
#define NLP_POOL_SLOTS 6u
#define NLP_CHANNELS 2u
#define NLP_WINDOW 2u
struct nlp_session;
struct nlp_slot {
    struct nlp_session *owner;
    struct nlp_header header;
    uint8_t bytes[NLP_PDU_MAX];
    uint32_t due_ms;
    uint16_t length;
    uint8_t role, channel, retries, sent;
};
struct nlp_pool {
    struct nlp_slot slot[NLP_POOL_SLOTS];
};
struct nlp_channel {
    const struct nlp_cipher *cipher;
    uint32_t counter, tx_seq, tx_acked, tx_limit, rx_ack, rx_delivered, rx_limit;
    uint32_t replay_top, last_ack_ms;
    uint64_t replay_bits;
    uint8_t ack_pending;
};
typedef int (*nlp_message_fn)(void *, struct nlp_session *, const struct nlp_frame *);
typedef void (*nlp_closed_fn)(void *, struct nlp_session *, unsigned reason);
struct nlp_session {
    struct nlp_pool *pool;
    struct nlp_channel channel[NLP_CHANNELS];
    nlp_pdu_fn send;
    nlp_message_fn message;
    nlp_closed_fn closed;
    void *context;
    uint32_t id, epoch, last_receive_ms, lease_ms, rto_ms;
    uint16_t local_node, remote_node, local_cap, remote_cap, payload_limit;
    uint8_t active, reliable_mask, window;
};
void nlp_session_init(struct nlp_session *, struct nlp_pool *);
int nlp_session_start(struct nlp_session *, uint32_t now_ms);
void nlp_session_close(struct nlp_session *, unsigned reason);
int nlp_session_send(struct nlp_session *, unsigned channel, unsigned kind, unsigned opcode,
                     unsigned priority, const void *, size_t, uint32_t now_ms);
int nlp_session_receive(struct nlp_session *, const uint8_t *, size_t, uint32_t now_ms);
void nlp_session_poll(struct nlp_session *, uint32_t now_ms);
#endif
