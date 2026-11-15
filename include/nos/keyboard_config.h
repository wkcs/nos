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
#define KEYBOARD_CONFIG_DESC_SIZE NLK_KB_CONFIG_DESC_SIZE
#define KEYBOARD_CONFIG_HARDWARE_SIZE NLK_KB_CONFIG_HARDWARE_SIZE
#define KEYBOARD_CONFIG_ID_SIZE NLK_KB_CONFIG_ID_SIZE
#define KEYBOARD_CONFIG_MAGIC 0x31434b4eUL

/* Each object kind has exactly one current on-disk layout. */
static inline uint16_t keyboard_config_version(uint8_t kind)
{
    switch (kind) {
    case KEYBOARD_CONFIG_KEYBOARD:
        return 5U;
    case KEYBOARD_CONFIG_ACTIVE:
        return 4U;
    case KEYBOARD_CONFIG_LIGHT:
    case KEYBOARD_CONFIG_MACRO:
        return 3U;
    default:
        return 0U;
    }
}

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
    char desc[KEYBOARD_CONFIG_DESC_SIZE];
} __attribute__((packed));

int keyboard_config_encode(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                           const char *name, const char *desc, uint8_t kind, uint8_t macro_id,
                           void *buffer, size_t capacity, size_t *length);
/* Persistent profiles retain only their bound macros; backups use encode(). */
int keyboard_config_encode_persistent(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                                      const char *name, const char *desc, uint8_t kind, uint8_t macro_id,
                                      void *buffer, size_t capacity, size_t *length);
int keyboard_config_macro_bound(const struct nlk_keyboard_ctx *ctx, uint8_t slot,
                                uint32_t hash, uint32_t name_hash);
int keyboard_config_decode(struct nlk_keyboard_ctx *ctx, const char *hardware,
                           uint8_t kind, uint8_t macro_id,
                           const void *buffer, size_t length);
/* Validate without changing runtime state; report whether macro bindings exist. */
int keyboard_config_validate(struct nlk_keyboard_ctx *ctx, const char *hardware,
                             uint8_t kind, uint8_t macro_id,
                             const void *buffer, size_t length, bool *macro_bindings);

struct keyboard_config_file_info {
    char id[KEYBOARD_CONFIG_ID_SIZE];
    char name[KEYBOARD_CONFIG_NAME_SIZE];
    char desc[KEYBOARD_CONFIG_DESC_SIZE];
    uint32_t size;
};

int keyboard_config_file_save(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, const char *name,
                              const char *desc, uint8_t kind, uint8_t macro_id);
int keyboard_config_activate_macros(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                    const char *dir, struct mutex *lock);
int keyboard_config_file_load(struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, uint8_t kind,
                              uint8_t macro_id);
int keyboard_config_file_next(const char *dir, const char *hardware, uint8_t kind,
                              uint32_t *cursor, struct keyboard_config_file_info *info);
int keyboard_config_find_by_hash(const char *dir, const char *hardware, uint32_t hash,
                                 char id[KEYBOARD_CONFIG_ID_SIZE]);
int keyboard_config_file_delete(const char *dir, const char *id, uint8_t kind);
/* Remove a library object, its resident cache, and bindings in all saved snapshots. */
int keyboard_config_macro_delete(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                  const char *dir, const char *id);
/* File IDs use the filesystem contract, independently of shorter macro metadata IDs. */
struct keyboard_config_macro_target {
    char file_id[KEYBOARD_CONFIG_ID_SIZE];
    struct nlk_kb_macro_info info;
};

/* Codec helpers return one when changed, zero when unchanged, or a negative errno. */
int keyboard_config_remove_macro(struct nlk_keyboard_ctx *ctx,
                                  const struct keyboard_config_macro_target *target);
int keyboard_config_strip_macro(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                 const struct keyboard_config_macro_target *target,
                                 void *buffer, size_t *length);

/* Returns 1 when written, 0 when unchanged, or a negative errno. */
int keyboard_config_snapshot_write(const char *dir, const void *buffer, size_t length);
int keyboard_config_active_save(const struct nlk_keyboard_ctx *ctx,
                                const char *hardware, const char *dir);
int keyboard_config_file_restore(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                 const char *dir, uint8_t kind);
int keyboard_config_file_rpc(struct nlk_keyboard_ctx *ctx, const char *hardware,
                             const char *dir, uint8_t op, const void *req,
                             uint16_t req_len, void *resp, uint16_t *resp_len);

int keyboard_config_manifest_build(const char *dir, const char *hardware, struct nlk_kb_manifest *manifest);
const char *keyboard_config_manifest_lookup_config(const struct nlk_kb_manifest *manifest, uint32_t hash);
const char *keyboard_config_manifest_lookup_macro(const struct nlk_kb_manifest *manifest, uint32_t hash);
int keyboard_config_manifest_update(struct nlk_kb_manifest *manifest, uint8_t kind, const char *id, int remove);

#endif
