/* Copyright (C) 2026 NOS contributors. Typed wireless control, not an AT/HCI byte tunnel. */
#ifndef NOS_NLP_RADIO_H
#define NOS_NLP_RADIO_H
#include "endpoint.h"
struct nlp_radio_params {
    uint32_t present, job, duration_ms, passkey;
    uint16_t max_results, source_cap;
    uint8_t role, address[6], address_type, method, security;
    uint8_t ssid[32], password[63], source_uid[16], ssid_length, password_length;
};
struct nlp_radio {
    struct nlp_endpoint *endpoint;
    uint32_t roles, job, owner, sequence;
    uint8_t bluetooth, state, results, result_limit;
    int (*control)(void *, unsigned opcode, const struct nlp_radio_params *);
    void (*cancel)(void *);
    void *context;
};
int nlp_radio_register(struct nlp_radio *, struct nlp_endpoint *, uint16_t id, int bluetooth,
                       uint32_t roles,
                       int (*control)(void *, unsigned, const struct nlp_radio_params *),
                       void (*cancel)(void *), void *context);
int nlp_radio_event(struct nlp_radio *, unsigned opcode, const uint8_t *tlvs, size_t);
#endif
