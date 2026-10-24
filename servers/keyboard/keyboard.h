/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_KEYBOARD_H
#define NOS_KEYBOARD_H
#include "config.h"
#define KB_BOOT_BYTES 8u
#define KB_NKRO_BYTES 33u
#define KB_REPORT_QUEUE 8u

struct kb_report {
    uint32_t sequence;
    uint8_t boot[KB_BOOT_BYTES];
    uint8_t nkro[KB_NKRO_BYTES];
    uint8_t consumer[2];
};
struct kb_key_state {
    struct kb_action action;
    uint32_t changed_ms, pressed_ms;
    uint8_t raw, stable, active, pending, oneshot;
};
struct keyboard {
    struct kb_config config;
    struct kb_key_state key[KB_KEYS];
    struct kb_report reports[KB_REPORT_QUEUE], current;
    uint32_t last_ms, sequence, overflows, local_events;
    uint32_t macro_changed_ms, caps_word_ms;
    uint8_t head, count, toggled_layers, oneshot, blocked, clock_valid;
    uint8_t macro_running, macro_index, macro_step, macro_on, caps_word;
    uint16_t macro_usage;
};
int kb_init(struct keyboard *keyboard, const struct kb_config *config);
/* pressed contains 61 active-high logical switch bits, not HC595 wire order.
 * Time is a wrapping monotonic millisecond counter, sampled at least every
 * 2^31 ms. Report queue overflow fails closed until all switches are released. */
int kb_scan(struct keyboard *keyboard, uint64_t pressed, uint32_t now_ms);
const struct kb_report *kb_report_peek(const struct keyboard *keyboard);
void kb_report_pop(struct keyboard *keyboard);
/* Configuration changes release all keys and require a physical all-up before
 * accepting new presses, so remapping a held key cannot leave host keys stuck. */
int kb_reconfigure(struct keyboard *keyboard, const struct kb_config *config);
void kb_release_all(struct keyboard *keyboard);
uint32_t kb_take_local_events(struct keyboard *keyboard);
unsigned kb_active_layers(const struct keyboard *keyboard);
#endif
