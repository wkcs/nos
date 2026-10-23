/* Copyright (C) 2026 NOS contributors. Signed staging API; never raw flash addresses. */
#ifndef NOS_NLP_FIRMWARE_H
#define NOS_NLP_FIRMWARE_H
#include "endpoint.h"
enum nlp_update_state {
    NLP_UPDATE_EMPTY,
    NLP_UPDATE_MANIFEST,
    NLP_UPDATE_RECEIVING,
    NLP_UPDATE_VERIFIED,
    NLP_UPDATE_PENDING,
    NLP_UPDATE_COMMITTED
};
struct nlp_update {
    uint64_t id;
    uint32_t image_size, accepted, durable;
    uint16_t manifest_size, manifest_offset;
    uint8_t state, digest[32], image_digest[32], manifest[512];
};
struct nlp_firmware_ops {
    void *context;
    const struct nlp_crypto *crypto;
    uint8_t board[16], component[16];
    uint32_t hardware, image_limit, erase_granule;
    uint64_t current_version, rollback_index;
    uint8_t mode;                             /* 0 staging only, 1 A/B, 2 recovery. */
    int (*allocate_id)(void *, uint64_t *);   /* Durable, never reused; no image erase. */
    int (*load)(void *, struct nlp_update *); /* NOT_FOUND means no prior update. */
    int (*checkpoint)(void *, const struct nlp_update *); /* Durable atomic metadata. */
    int (*signature)(void *, uint32_t key_id, const uint8_t *, size_t, const uint8_t signature[64]);
    int (*prepare)(void *, uint32_t image_size);
    int (*read)(void *, uint32_t, void *, size_t);
    int (*write)(void *, uint32_t, const void *, size_t);
    int (*recover)(void *, uint32_t durable_offset);
    int (*verify_image)(void *, uint32_t image_size, const uint8_t sha256[32]);
    int (*abort)(void *, uint64_t update_id);
    /* Optional. Must be durable and idempotent by update_id, independently
     * enforce signature/boot compatibility, and never claim a fake activation. */
    int (*apply)(void *, uint64_t update_id);
    int (*confirm)(void *, uint64_t update_id);
};
struct nlp_firmware {
    struct nlp_firmware_ops ops;
    struct nlp_update update;
    uint8_t valid;
};
int nlp_firmware_register(struct nlp_firmware *, struct nlp_endpoint *, uint16_t,
                          const struct nlp_firmware_ops *);
#endif
