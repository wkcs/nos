/* Copyright (C) 2026 NOS contributors. Optional authenticated endpoint backend. */
#ifndef NOS_NLP_AUTH_H
#define NOS_NLP_AUTH_H
#include "endpoint.h"
struct nlp_auth {
    struct nlp_noise noise;
    struct nlp_cipher cipher[NLP_SESSIONS][NLP_CHANNELS];
    const struct nlp_crypto *crypto;
    uint8_t static_key[32], peer_pin[32];
    uint8_t request[NLP_PAYLOAD_MAX], reply[NLP_PDU_MAX];
    uint16_t request_size, request_opcode, reply_size;
    uint32_t request_id, session_id;
};
int nlp_auth_init(struct nlp_auth *, const struct nlp_crypto *, const uint8_t static_key[32],
                  const uint8_t peer_pin[32], uint8_t uid[16]);
void nlp_auth_reset(struct nlp_auth *);
int nlp_auth_control(struct nlp_endpoint *, const struct nlp_frame *);
int nlp_auth_channel(struct nlp_endpoint *, struct nlp_binding *, unsigned index);
int nlp_auth_join_mac(struct nlp_endpoint *, const struct nlp_header *, const uint8_t *, size_t,
                      int transmit, uint8_t out[32]);
int nlp_auth_join_verify(struct nlp_endpoint *, const struct nlp_frame *);
#endif
