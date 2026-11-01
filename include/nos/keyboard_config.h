/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Versioned keyboard configuration object codec.
 */
#ifndef __NOS_KEYBOARD_CONFIG_H__
#define __NOS_KEYBOARD_CONFIG_H__

#include <stddef.h>
#include <stdint.h>
#include <nos/nlk_caps.h>

enum keyboard_config_kind {
    KEYBOARD_CONFIG_KEYBOARD = NLK_KB_CONFIG_KEYBOARD,
    KEYBOARD_CONFIG_LIGHT = NLK_KB_CONFIG_LIGHT,
    KEYBOARD_CONFIG_MACRO = NLK_KB_CONFIG_MACRO,
    KEYBOARD_CONFIG_ACTIVE = 4U,
};

#define KEYBOARD_CONFIG_NAME_SIZE NLK_KB_CONFIG_NAME_SIZE
#define KEYBOARD_CONFIG_HARDWARE_SIZE NLK_KB_CONFIG_HARDWARE_SIZE
#define KEYBOARD_CONFIG_ID_SIZE NLK_KB_CONFIG_ID_SIZE
#define KEYBOARD_CONFIG_MAGIC 0x31434b4eUL

struct keyboard_config_header {
    uint32_t magic;
    uint16_t version;
    uint8_t kind;
    uint8_t key_count;
    uint8_t layer_count;
    uint8_t reserved[3];
    uint32_t length;
    uint32_t checksum;
    char hardware[KEYBOARD_CONFIG_HARDWARE_SIZE];
    char name[KEYBOARD_CONFIG_NAME_SIZE];
} __attribute__((packed));

int keyboard_config_encode(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                           const char *name, uint8_t kind, uint8_t macro_id,
                           void *buffer, size_t capacity, size_t *length);
int keyboard_config_decode(struct nlk_keyboard_ctx *ctx, const char *hardware,
                           uint8_t kind, uint8_t macro_id,
                           const void *buffer, size_t length);

struct keyboard_config_file_info {
    char id[KEYBOARD_CONFIG_ID_SIZE];
    char name[KEYBOARD_CONFIG_NAME_SIZE];
    uint32_t size;
};

int keyboard_config_file_save(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, const char *name,
                              uint8_t kind, uint8_t macro_id);
int keyboard_config_file_load(struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, uint8_t kind,
                              uint8_t macro_id);
int keyboard_config_file_next(const char *dir, const char *hardware, uint8_t kind,
                              uint32_t *cursor, struct keyboard_config_file_info *info);
int keyboard_config_file_delete(const char *dir, const char *id, uint8_t kind);
int keyboard_config_active_save(const struct nlk_keyboard_ctx *ctx,
                                const char *hardware, const char *dir);
int keyboard_config_file_restore(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                 const char *dir, uint8_t kind);
int keyboard_config_file_rpc(struct nlk_keyboard_ctx *ctx, const char *hardware,
                             const char *dir, uint8_t op, const void *req,
                             uint16_t req_len, void *resp, uint16_t *resp_len);

#endif
