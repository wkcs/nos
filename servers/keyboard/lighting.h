/* Copyright (C) 2026 NOS contributors. Bounded per-key lighting and durable preferences. */
#ifndef NOS_KEYBOARD_LIGHTING_H
#define NOS_KEYBOARD_LIGHTING_H
#include "config.h"
#include "lighting_fx.h"
#include "../../boards/nk60_v2/hardware.h"
#define KB_LIGHT_SETTINGS_BYTES 224u
#define KB_LIGHT_PREFERENCES_BYTES (KB_LIGHT_SETTINGS_BYTES + KB_FX_PROGRAM_BYTES)
#define KB_LIGHT_MODES KB_RGB_MODES
#define KB_LIGHT_CHANNEL_LIMIT 16u
#define KB_LIGHT_INTERVAL_MS 100u
#define KB_LIGHT_INDICATORS 1u
#define KB_LIGHT_MODE_MASK ((1u << KB_LIGHT_MODES) - 1u)
enum kb_light_mode {
    KB_LIGHT_OFF,
    KB_LIGHT_STATIC,
    KB_LIGHT_LAYER,
    KB_LIGHT_BREATHE,
    KB_LIGHT_RAINBOW,
    KB_LIGHT_WAVE,
    KB_LIGHT_REACTIVE,
    KB_LIGHT_CUSTOM
};
struct kb_light_settings {
    struct nk60_rgb color, custom[KB_KEYS];
    uint16_t idle_seconds, current_ma;
    uint8_t speed, flags;
};
struct kb_lighting {
    struct kb_light_settings settings;
    struct kb_fx fx;
    uint8_t reactive[KB_KEYS];
    uint64_t pressed;
    uint32_t last_activity_ms, last_frame_ms;
    uint16_t estimated_ma;
};
void kb_light_defaults(struct kb_light_settings *);
int kb_light_validate(const struct kb_light_settings *);
int kb_light_encode(const struct kb_light_settings *, uint8_t out[KB_LIGHT_SETTINGS_BYTES]);
int kb_light_decode(struct kb_light_settings *, const uint8_t *, size_t);
int kb_light_preferences_encode(const struct kb_lighting *,
                                uint8_t out[KB_LIGHT_PREFERENCES_BYTES]);
int kb_light_preferences_decode(struct kb_lighting *, const uint8_t *, size_t);
void kb_light_activity(struct kb_lighting *, uint64_t debounced, uint32_t now);
/* Returns 1 if power is allowed; enforces suspend/off/idle and all output limits. */
int kb_light_render(struct kb_lighting *, unsigned mode, uint8_t brightness, unsigned layers,
                    unsigned host_leds, int configured, int suspended, uint32_t now,
                    struct nk60_rgb out[KB_KEYS]);
#endif
