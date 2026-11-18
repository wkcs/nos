/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_KEYBOARD_CONFIG_SERVICE_H
#define NOS_KEYBOARD_CONFIG_SERVICE_H

#include <nos/keyboard_definitions.h>
#include <nos/keyboard_document.h>
#include <nos/keyboard_object_store.h>
#include <kernel/mutex.h>

#define KB_CONFIG_BODY_MAX KB_STORE_MAX_BODY
#define KB_CONFIG_TTL_MS 60000U

struct kb_config_operation {
    uint64_t id, nonce, fingerprint;
    uint16_t owner;
    uint32_t session;
    struct nlk_bearer *ingress;
    uint8_t type, verb, kind, state;
    uint32_t order;
    uint64_t generation;
    int16_t status;
    uint64_t expected;
    struct kb_store_ref ref;
    char target[33];
};

struct keyboard_config_service {
    struct nlk_keyboard_ctx *keyboard;
    struct kb_action_runtime action_runtime;
    struct mutex *keyboard_lock;
    int (*check_storage)(void *context);
    void *storage_context;
    struct kb_store store;
    struct kb_store_selection selection;
    uint64_t generation, base_epoch, light_epoch, boot_id, counter;
    uint64_t lease, draft_revision, source_revision, edit_epoch;
    uint32_t lease_deadline, now, first_dirty, last_dirty, last_write, retry_at;
    uint8_t kind, retry_count, storage_state;
    int16_t storage_error;
    bool initialized, ready, read_only, pending, write_error, draft_applied, draft_enabled,
        draft_synced;
    char source[33];
    uint16_t edit_owner;
    uint32_t edit_session;
    struct nlk_bearer *edit_ingress;
    uint8_t *draft, *active_bindings;
    size_t draft_length, draft_capacity, bindings_length;
    struct kb_config_operation operations[4];
    uint8_t operation_next;
    uint32_t order_counter;
    struct {
        uint32_t order;
        uint8_t verb;
        char target[24];
    } action_queue[8];
    volatile uint8_t action_head, action_tail;
};

int keyboard_config_service_init(struct keyboard_config_service *s,
                                 struct nlk_keyboard_ctx *keyboard, struct mutex *lock);
void keyboard_config_service_set_storage_error(struct keyboard_config_service *s, int error);
void keyboard_config_service_poll(struct keyboard_config_service *s, uint32_t now);
int keyboard_config_service_queue(struct keyboard_config_service *s, uint8_t verb,
                                  const char *target);

#endif
