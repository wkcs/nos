/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_CAPABILITIES_H
#define NOS_NLP_CAPABILITIES_H
#include "endpoint.h"
struct nlp_keyboard {
    struct nlp_endpoint *endpoint;
    uint8_t state[52];
    uint32_t sequence, sent_sequence[NLP_SESSIONS], subscription_id[NLP_SESSIONS],
        refresh[NLP_SESSIONS], lease[NLP_SESSIONS], last[NLP_SESSIONS];
    uint8_t subscribed[NLP_SESSIONS], blocked[NLP_SESSIONS];
    int (*set_leds)(void *, uint8_t);
    int (*set_owner)(void *, const uint8_t uid[16], uint16_t cap, unsigned mode);
    void *context;
};
int nlp_keyboard_register(struct nlp_keyboard *, struct nlp_endpoint *, uint16_t id);
void nlp_keyboard_update(struct nlp_keyboard *, const uint8_t nkro[33], uint16_t consumer,
                         uint64_t monotonic_us);
void nlp_keyboard_poll(struct nlp_keyboard *);
struct nlp_log_record {
    uint8_t bytes[NLP_PAYLOAD_MAX];
    uint16_t length;
    uint32_t seq;
};
struct nlp_log {
    struct nlp_endpoint *endpoint;
    struct nlp_log_record records[4];
    uint32_t sequence, dropped, subscription_id[NLP_SESSIONS], cursor[NLP_SESSIONS],
        stamp[NLP_SESSIONS];
    uint16_t rate[NLP_SESSIONS], components[NLP_SESSIONS][4];
    uint8_t subscribed[NLP_SESSIONS], mask[NLP_SESSIONS], component_count[NLP_SESSIONS];
};
int nlp_log_register(struct nlp_log *, struct nlp_endpoint *, uint16_t id);
int nlp_log_write(struct nlp_log *, unsigned level, uint16_t component, const char *, size_t,
                  uint64_t monotonic_us);
void nlp_log_poll(struct nlp_log *);
#define NLP_INPUT_SOURCES 4u
struct nlp_input_source {
    uint8_t uid[16], bitmap[32], modifiers, active, blocked;
    uint64_t boot;
    uint16_t cap, controls, buttons, map[16];
    uint32_t binding, seq, relative_seq, last_ms, lease_ms;
};
struct nlp_inputs {
    struct nlp_input_source source[NLP_INPUT_SOURCES];
};
int nlp_input_bind(struct nlp_inputs *, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                   uint32_t binding, uint16_t controls, const uint16_t usages[16],
                   uint32_t lease_ms);
int nlp_input_state(struct nlp_inputs *, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                    const uint8_t state[16], uint32_t now_ms);
int nlp_input_relative(struct nlp_inputs *, const uint8_t uid[16], uint64_t boot, uint16_t cap,
                       const uint8_t event[16], uint32_t now_ms,
                       int (*action)(void *, uint16_t control, int16_t delta), void *);
void nlp_input_drop(struct nlp_inputs *, const uint8_t uid[16], uint16_t cap);
void nlp_input_merge(struct nlp_inputs *, uint32_t now_ms, uint8_t nkro[33]);
struct nlp_display_ops {
    void *context;
    uint16_t width, height, formats, modes, tile_bytes, max_tiles, interval_ms;
    int (*begin)(void *, unsigned atomic);
    int (*tile)(void *, uint16_t, uint16_t, uint16_t, uint16_t, unsigned, const uint8_t *, size_t);
    int (*end)(void *);
    void (*abort)(void *);
    int (*power)(void *, int enabled);
};
struct nlp_display {
    struct nlp_display_ops ops;
    uint32_t frame, crc, owner, last_frame_ms;
    uint16_t expected, next;
    uint8_t active, presented;
};
int nlp_display_register(struct nlp_display *, struct nlp_endpoint *, uint16_t,
                         const struct nlp_display_ops *);
#endif
