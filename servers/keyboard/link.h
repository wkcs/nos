/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_LINK_H
#define NOS_NK60_LINK_H
#include "nlp/capabilities.h"
#include "nlp/keymap.h"
struct nk60_service;
struct nk60_link {
    struct nlp_endpoint endpoint;
    struct nlp_stream stream;
    struct nlp_keyboard keyboard;
    struct nlp_log log;
    struct nlp_keymap keymap;
    uint8_t unlocked_uid[16], static_rgb[3], last_unlocked;
    uint64_t unlocked_boot;
    uint8_t tx[NLP_PDU_MAX + 2], rx[64];
    uint16_t tx_size, tx_at;
    uint8_t rx_size, rx_at, ready, configured, suspended;
    uint32_t generation;
};
int nk60_link_capabilities(struct nk60_service *);
int nk60_link_authorized(struct nk60_service *, const struct nlp_binding *);
int nk60_link_init(struct nk60_service *);
void nk60_link_step(struct nk60_service *);
#endif
