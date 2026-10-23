/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_TERMINAL_H
#define NOS_NLP_TERMINAL_H
#include "endpoint.h"
struct nlp_terminal_ops {
    void *context;
    int (*open)(void *, uint16_t cols, uint16_t rows, const char *term);
    int (*resize)(void *, uint16_t cols, uint16_t rows);
    int (*interrupt)(void *, unsigned signal);
    int (*write)(void *, const uint8_t *, size_t);
    /* read returns 0 + length, BUSY when empty, DISCONNECTED at EOF. */
    int (*read)(void *, uint8_t *, size_t, size_t *);
    int (*exit_status)(void *, int32_t *);
    void (*close)(void *);
};
struct nlp_terminal {
    struct nlp_endpoint *endpoint;
    struct nlp_terminal_ops ops;
    uint8_t pending[NLP_PAYLOAD_MAX];
    uint16_t pending_size;
    uint32_t owner, id, next_id;
    int32_t status;
    uint8_t active, exited;
};
int nlp_terminal_register(struct nlp_terminal *, struct nlp_endpoint *, uint16_t,
                          const struct nlp_terminal_ops *);
void nlp_terminal_poll(struct nlp_terminal *);
#endif
