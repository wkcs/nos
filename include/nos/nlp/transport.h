/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_TRANSPORT_H
#define NOS_NLP_TRANSPORT_H
#include "wire.h"
#define NLP_FRAMING_TCP 0u
#define NLP_FRAMING_UART 1u
#define NLP_ENCODED_MAX (NLP_PDU_MAX + 2u + (NLP_PDU_MAX + 2u) / 254u + 2u)
typedef int (*nlp_pdu_fn)(void *, const uint8_t *, size_t);
struct nlp_stream {
    uint8_t bytes[NLP_ENCODED_MAX];
    uint16_t used, expected;
    uint32_t last_ms, errors;
    uint8_t mode, failed, discard, ready;
};
void nlp_stream_init(struct nlp_stream *, unsigned mode);
int nlp_stream_feed(struct nlp_stream *, const uint8_t *, size_t, size_t *consumed, uint32_t now_ms,
                    nlp_pdu_fn, void *);
void nlp_stream_timeout(struct nlp_stream *, uint32_t now_ms);
int nlp_stream_wrap(unsigned mode, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
int nlp_cobs_encode(const uint8_t *, size_t, uint8_t *, size_t, size_t *);
int nlp_cobs_decode(const uint8_t *, size_t, uint8_t *, size_t, size_t *);
struct nlp_mailbox {
    uint8_t queue[2][NLP_PDU_MAX];
    uint16_t lengths[2];
    uint64_t boot_id;
    uint32_t sequence;
    uint8_t head, count;
};
void nlp_mailbox_init(struct nlp_mailbox *, uint64_t boot_id);
int nlp_mailbox_enqueue(struct nlp_mailbox *, const uint8_t *, size_t);
int nlp_mailbox_read(struct nlp_mailbox *, unsigned reg, uint8_t *, size_t);
/* Invoke write only AFTER a complete I2C/SPI transaction, never from a partial DMA. */
int nlp_mailbox_write(struct nlp_mailbox *, unsigned reg, const uint8_t *, size_t, nlp_pdu_fn,
                      void *);
#define NLP_CAN_SLOTS 2u
struct nlp_can_slot {
    uint8_t bytes[NLP_PDU_MAX], last[64];
    uint32_t started_ms;
    uint16_t used, expected, next;
    uint8_t source, destination, transfer, subtype, active, last_size;
};
struct nlp_can {
    struct nlp_can_slot slot[NLP_CAN_SLOTS];
    uint32_t generation;
    uint8_t address;
};
int nlp_can_id(unsigned priority, unsigned source, unsigned destination, unsigned transfer,
               unsigned subtype, uint32_t *id);
uint32_t nlp_can_commission_reply_id(uint8_t byte, uint8_t token);
void nlp_can_reset(struct nlp_can *, uint8_t address);
int nlp_can_receive(struct nlp_can *, uint32_t id, const uint8_t *, size_t, uint32_t now_ms,
                    nlp_pdu_fn, void *);
int nlp_can_fragment(const uint8_t *, size_t, unsigned mtu, unsigned index, uint8_t out[64],
                     size_t *written);
#endif
