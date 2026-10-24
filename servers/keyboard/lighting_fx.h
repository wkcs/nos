/* Copyright (C) 2026 NOS contributors. Declarative, bounded lighting composition. */
#ifndef NOS_KEYBOARD_LIGHTING_FX_H
#define NOS_KEYBOARD_LIGHTING_FX_H
#include "keyboard.h"
#include "../../boards/nk60_v2/hardware.h"
#define KB_FX_RULES 12u
#define KB_FX_INSTANCES 16u
#define KB_FX_RULE_BYTES 32u
#define KB_FX_PROGRAM_BYTES (16u + KB_FX_RULES * KB_FX_RULE_BYTES)
#define KB_FX_ENABLED 1u
#define KB_FX_DARK_BASE 2u
#define KB_FX_SINGLE 1u
#define KB_FX_REVERSE 2u
#define KB_FX_VERTICAL 4u
#define KB_FX_ANY_MODS 8u
#define KB_FX_NO_MODS 16u
#define KB_FX_NO_KEY 255u
enum kb_fx_trigger {
    KB_FX_ALWAYS,
    KB_FX_PRESS,
    KB_FX_RELEASE,
    KB_FX_TAP,
    KB_FX_DOUBLE_TAP,
    KB_FX_HELD,
    KB_FX_HOLD_DECIDED,
    KB_FX_LAYER_ENTER,
    KB_FX_LAYER_LEAVE,
    KB_FX_LAYER_ACTIVE,
    KB_FX_LED_ON,
    KB_FX_LED_OFF,
    KB_FX_LED_ACTIVE,
    KB_FX_CAPS_ON,
    KB_FX_CAPS_OFF,
    KB_FX_CAPS_ACTIVE,
    KB_FX_MACRO_START,
    KB_FX_MACRO_END,
    KB_FX_MACRO_ACTIVE,
    KB_FX_CONNECT,
    KB_FX_DISCONNECT,
    KB_FX_IDLE,
    KB_FX_WAKE,
    KB_FX_MODS_ON,
    KB_FX_MODS_OFF,
    KB_FX_MODS_ACTIVE,
    KB_FX_TRIGGER_COUNT
};
enum kb_fx_match {
    KB_FX_ANY,
    KB_FX_KEY,
    KB_FX_USAGE,
    KB_FX_ACTION,
    KB_FX_LAYER,
    KB_FX_HOST_LED,
    KB_FX_MACRO,
    KB_FX_CONSUMER,
    KB_FX_MODIFIERS,
    KB_FX_MATCH_COUNT
};
enum kb_fx_target { KB_FX_SELF, KB_FX_MASK, KB_FX_ALL };
enum kb_fx_effect {
    KB_FX_SOLID,
    KB_FX_FADE,
    KB_FX_PULSE,
    KB_FX_BREATHE,
    KB_FX_RAINBOW,
    KB_FX_WAVE,
    KB_FX_RIPPLE,
    KB_FX_EFFECT_COUNT
};
enum kb_fx_blend { KB_FX_REPLACE, KB_FX_ADD, KB_FX_MAX, KB_FX_MULTIPLY };
struct kb_fx_instance {
    uint32_t started;
    uint8_t rule, key, active, reserved;
};
struct kb_fx {
    uint8_t program[KB_FX_PROGRAM_BYTES];
    struct kb_fx_instance instances[KB_FX_INSTANCES];
    uint32_t tap_ms[KB_KEYS], dropped, epoch_ms, now_ms;
    uint32_t layer_ms[KB_LAYERS], led_ms[5], caps_ms, mods_ms;
    uint64_t tap_valid;
    const struct keyboard *keyboard;
    uint8_t layers, leds, mods, caps, connected, idle, context_valid, victim_cursor;
};
void kb_fx_defaults(struct kb_fx *);
int kb_fx_validate(const uint8_t *, size_t);
int kb_fx_set(struct kb_fx *, const uint8_t *, size_t);
void kb_fx_clear(struct kb_fx *);
void kb_fx_keyboard_event(void *, const struct kb_event *);
void kb_fx_observe(struct kb_fx *, const struct keyboard *, unsigned layers, unsigned leds,
                   int connected, uint32_t last_activity, uint32_t now);
void kb_fx_render(struct kb_fx *, uint32_t now, struct nk60_rgb out[KB_KEYS]);
unsigned kb_fx_interval(const struct kb_fx *);
#endif
