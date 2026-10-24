/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_KEYBOARD_CONFIG_H
#define NOS_KEYBOARD_CONFIG_H
#include <stddef.h>
#include <stdint.h>
#include "keycodes.h"

#define KB_KEYS 61u
#define KB_RGB_MODES 8u
#define KB_LAYERS 4u
#define KB_MACROS 4u
#define KB_MACRO_STEPS 8u
#define KB_LAYER_PN 1u
#define KB_LAYER_FN 2u
#define KB_CONFIG_WIRE_BYTES 1056u
#define KB_CONFIG_RECORD_BYTES (KB_CONFIG_WIRE_BYTES + 24u)
#define KB_CONFIG_SLOT_BYTES 4096u
#define KB_CONFIG_EXTRA_BYTES 640u
#define KB_CONFIG_RECORD_MAX_BYTES (KB_CONFIG_RECORD_BYTES + KB_CONFIG_EXTRA_BYTES)

enum kb_error { KB_EIO = 5, KB_EBUSY = 16, KB_EINVAL = 22, KB_ENOSPC = 28 };
enum kb_action_type {
    KB_NONE,
    KB_TRANSPARENT,
    KB_KEY,
    KB_MODIFIER,
    KB_MOMENTARY,
    KB_TOGGLE,
    KB_MOD_TAP,
    KB_LAYER_TAP,
    KB_ONESHOT,
    KB_CONSUMER,
    KB_MACRO,
    KB_LOCAL,
    KB_GRAVE_ESCAPE
};
enum kb_local_action {
    KB_LIGHT_NEXT = 0,
    KB_LIGHT_UP,
    KB_LIGHT_DOWN,
    KB_NKRO_TOGGLE,
    KB_CAPS_WORD
};
struct kb_action {
    uint8_t type, parameter;
    uint16_t code;
};
struct kb_config {
    uint16_t debounce_ms, tapping_ms, macro_ms;
    uint8_t layers, nkro, brightness, rgb_mode;
    uint16_t reserved;
    struct kb_action map[KB_LAYERS][KB_KEYS];
    uint8_t macro_length[KB_MACROS];
    uint16_t macros[KB_MACROS][KB_MACRO_STEPS];
};
/* NOR callbacks return zero only for complete success; erase is one 4 KiB slot.
 * write must support byte/page-safe programming and must not erase implicitly. */
struct kb_store {
    void *context;
    int (*read)(void *, uint32_t offset, void *, size_t);
    int (*erase)(void *, uint32_t offset, size_t);
    int (*write)(void *, uint32_t offset, const void *, size_t);
};
void kb_default_config(struct kb_config *config);
int kb_config_validate(const struct kb_config *config);
int kb_config_encode(const struct kb_config *config, uint8_t *data, size_t bytes);
int kb_config_decode(struct kb_config *config, const uint8_t *data, size_t bytes);
uint32_t kb_crc32(const void *data, size_t bytes);
/* Two NOR slots at base and base+4096; base must be 4 KiB aligned. Empty or
 * corrupt media returns -KB_EIO and leaves config/generation unchanged. */
int kb_config_load(const struct kb_store *store, uint32_t base, struct kb_config *config,
                   uint32_t *generation);
int kb_config_save(const struct kb_store *store, uint32_t base, const struct kb_config *config,
                   uint32_t *generation);
/* Version2 record includes an opaque, CRC-covered extension in the same atomic
 * commit. Canonical keymap wire remains1056 bytes. Version1 loads with size0.
 * load: *extra_size is capacity on entry, actual bytes on success. NULL/NULL
 * ignores the extension. Legacy save preserves the newest extension. */
int kb_config_load_extended(const struct kb_store *, uint32_t base, struct kb_config *,
                            uint32_t *generation, uint8_t *extra, size_t *extra_size);
int kb_config_save_extended(const struct kb_store *, uint32_t base, const struct kb_config *,
                            uint32_t *generation, const uint8_t *extra, size_t extra_size);
#endif
