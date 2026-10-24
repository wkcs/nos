/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_LINK_H
#define NOS_NK60_LINK_H
#include "nlp/capabilities.h"
#include "nlp/keymap.h"
#include "config.h"
#include "../../boards/nk60_v2/hardware.h"
struct nk60_service;
struct nk60_link {
    struct nlp_endpoint endpoint;
    struct nlp_stream stream;
    struct nlp_keyboard keyboard;
    struct nlp_log log;
    struct nlp_keymap keymap;
    uint8_t unlocked_uid[16], last_unlocked;
    struct nk60_rgb frame[KB_KEYS];
    uint64_t frame_received, frame_id;
    uint32_t frame_owner, frame_generation, frame_next, frame_deadline;
    uint8_t frame_active;
    uint64_t unlocked_boot;
    uint8_t tx[NLP_PDU_MAX + 2], rx[64];
    uint16_t tx_size, tx_at;
    uint8_t rx_size, rx_at, ready, configured, suspended;
    uint32_t generation;
};
int nk60_link_capabilities(struct nk60_service *);
int nk60_link_authorized(struct nk60_service *, const struct nlp_binding *);
int nk60_link_init(struct nk60_service *);
int nk60_link_device_register(struct nk60_service *);
void nk60_light_disconnect(void *, uint32_t);
void nk60_light_poll(struct nk60_service *);
void nk60_link_step(struct nk60_service *);
#endif
