/* Copyright (C) 2026 NOS contributors. Standard button/encoder module provider. */
#ifndef NOS_NLP_INPUT_DEVICE_H
#define NOS_NLP_INPUT_DEVICE_H
#include "endpoint.h"
struct nlp_control {
    uint16_t id, resolution;
    uint8_t type;
};
struct nlp_input_device {
    struct nlp_endpoint *endpoint;
    struct nlp_control controls[16];
    uint32_t sequence, relative_sequence;
    uint16_t buttons, button_mask, encoder_mask;
    uint8_t count;
    uint32_t subscription[NLP_SESSIONS], binding[NLP_SESSIONS], refresh[NLP_SESSIONS],
        last[NLP_SESSIONS];
    uint16_t mask[NLP_SESSIONS];
};
int nlp_input_device_register(struct nlp_input_device *, struct nlp_endpoint *, uint16_t,
                              const struct nlp_control *, unsigned count);
int nlp_input_device_buttons(struct nlp_input_device *, uint16_t buttons);
/* BUSY means keep/retry this increment locally; never replay it across a session. */
int nlp_input_device_rotate(struct nlp_input_device *, uint16_t control, int16_t delta);
void nlp_input_device_poll(struct nlp_input_device *);
#endif
