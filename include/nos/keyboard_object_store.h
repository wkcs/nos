/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#ifndef NOS_KEYBOARD_OBJECT_STORE_H
#define NOS_KEYBOARD_OBJECT_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define KB_STORE_MOUNT "/data"
#define KB_STORE_DIR KB_STORE_MOUNT "/keyboard"
#define KB_STORE_ROOT_PATH KB_STORE_DIR "/config.nkc"
#define KB_STORE_ROOT_TEMP KB_STORE_DIR "/config.tmp"
#define KB_STORE_CATALOG_DIR KB_STORE_DIR "/_catalog"
#define KB_STORE_BASE_DIR KB_STORE_DIR "/base"
#define KB_STORE_LIGHT_DIR KB_STORE_DIR "/light"
#define KB_STORE_MACRO_DIR KB_STORE_DIR "/macro"

#define KB_STORE_MAX_OBJECTS 24U
#define KB_STORE_MAX_BODY 8192U
#define KB_STORE_ID_SIZE 24U
#define KB_STORE_REF_ID_SIZE 33U
#define KB_STORE_CURRENT_REVISION UINT64_MAX

enum kb_store_kind { KB_STORE_BASE = 1, KB_STORE_LIGHT = 2, KB_STORE_MACRO = 3 };
struct kb_store_ref {
    uint64_t revision;
    char id[KB_STORE_REF_ID_SIZE];
};
struct kb_store_selection {
    struct kb_store_ref base;
    struct kb_store_ref light;
    bool light_enabled;
};
struct kb_store_entry {
    uint64_t revision;
    uint16_t length;
    uint8_t kind;
    char id[KB_STORE_ID_SIZE];
};
struct kb_store {
    struct kb_store_selection selection;
    uint64_t sequence;
    uint64_t catalog_revision;
    uint8_t count;
    bool ready;
    struct kb_store_entry entries[KB_STORE_MAX_OBJECTS];
};
/* Serialized worker use only. Failed staging preserves the published store.
 * Ambiguous publication is reconciled from disk; if neither root can be verified,
 * ready is cleared and reads/mutations/GC require successful kb_store_open(). */
int kb_store_open(struct kb_store *store);
/* Reserve a durable boot epoch; call once before accepting editor leases. */
int kb_store_begin_session(struct kb_store *store);
int kb_store_seed(struct kb_store *store, const void *base, uint16_t base_length, const void *light,
                  uint16_t light_length);
const struct kb_store_entry *kb_store_find(const struct kb_store *store, uint8_t kind,
                                           const char *id);
/* Returns bytes read, or negative errno. Exact historical revisions remain readable.
 * NULL buffer, zero length/offset queries the verified full body length. */
int kb_store_read(const struct kb_store *store, uint8_t kind, const char *id, uint64_t revision,
                  uint32_t offset, void *buffer, size_t length);
/* Also return the body size from the same verified read. */
int kb_store_read_page(const struct kb_store *store, uint8_t kind, const char *id,
                       uint64_t revision, uint32_t offset, void *buffer, size_t length,
                       uint32_t *body_size);
/* Optional selection merges current runtime state; saving base/light then activates it. */
int kb_store_save(struct kb_store *store, uint8_t kind, const char *id, uint64_t expected_revision,
                  const void *body, uint16_t length, const struct kb_store_selection *selection);
int kb_store_delete(struct kb_store *store, uint8_t kind, const char *id,
                    uint64_t expected_revision);
int kb_store_persist_selection(struct kb_store *store, const struct kb_store_selection *selection);
/* Run between worker operations; old unpinned revisions become unavailable. */
int kb_store_gc(struct kb_store *store);
#endif
