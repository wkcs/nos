/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_KEYMAP_H
#define NOS_NLP_KEYMAP_H
#include "endpoint.h"
#define NLP_KEYMAP_BYTES 1056u
struct nlp_keymap_ops {
    void *context;
    uint32_t (*generation)(void *);
    int (*snapshot)(void *, uint8_t out[NLP_KEYMAP_BYTES]);
    int (*validate)(void *, const uint8_t[NLP_KEYMAP_BYTES]);
    int (*commit)(void *, const uint8_t[NLP_KEYMAP_BYTES], int persist, unsigned *state);
    unsigned (*persistence)(void *); /* 0 volatile, 1 pending, 2 persisted. */
    int (*authorized)(void *, const struct nlp_binding *);
};
struct nlp_keymap {
    struct nlp_keymap_ops ops;
    uint8_t bytes[NLP_KEYMAP_BYTES], digest[32];
    uint64_t id;
    uint32_t next_id, owner, expected, committed_generation;
    uint16_t offset;
    uint8_t active, validated, persist, state;
};
int nlp_keymap_register(struct nlp_keymap *, struct nlp_endpoint *, uint16_t,
                        const struct nlp_keymap_ops *);
#endif
