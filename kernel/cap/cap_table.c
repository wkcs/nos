/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Capability Table & Generation-Counted Handle Implementation
 */

#define pr_fmt(fmt) "[CAP]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/cap.h>
#include <uapi/nos/abi.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <string.h>

#define NOS_CAP_DYNAMIC_START_SLOT (NOS_CAP_SLOT_VFS_EP + 1U)

static uint16_t next_generation(uint16_t current_gen)
{
    uint16_t next = (uint16_t)(current_gen + 1U);
    return (next == 0U) ? 1U : next;
}

void nos_cap_table_init(struct nos_cap_table *table)
{
    uint32_t i;

    if (table == NULL) {
        return;
    }

    spin_lock_init(&table->lock);
    for (i = 0; i < NOS_CAP_MAX_SLOTS; i++) {
        table->slots[i].type = NOS_CAP_NONE;
        table->slots[i].gen = 1U;
        table->slots[i].rights = 0U;
        table->slots[i].badge = 0U;
        table->slots[i].obj = NULL;
    }
}

void nos_cap_table_destroy(struct nos_cap_table *table)
{
    uint32_t i;

    if (table == NULL) {
        return;
    }

    spin_lock_irq(&table->lock);
    for (i = 0; i < NOS_CAP_MAX_SLOTS; i++) {
        table->slots[i].type = NOS_CAP_NONE;
        table->slots[i].gen = next_generation(table->slots[i].gen);
        table->slots[i].rights = 0U;
        table->slots[i].badge = 0U;
        table->slots[i].obj = NULL;
    }
    spin_unlock_irq(&table->lock);
}

int nos_cap_alloc_at(struct nos_cap_table *table,
                     uint16_t index,
                     enum nos_cap_type type,
                     void *obj,
                     nos_cap_rights_t rights,
                     uint32_t badge,
                     nos_handle_t *out_handle)
{
    struct nos_cap_slot *slot;
    nos_handle_t h;

    if (table == NULL || type == NOS_CAP_NONE || obj == NULL) {
        return -EINVAL;
    }
    if (index == 0U || index >= NOS_CAP_MAX_SLOTS) {
        return -EINVAL;
    }

    spin_lock_irq(&table->lock);
    slot = &table->slots[index];
    if (slot->type != NOS_CAP_NONE) {
        spin_unlock_irq(&table->lock);
        return -EBUSY;
    }

    if (slot->gen == 0U) {
        slot->gen = 1U;
    }
    slot->type = (uint16_t)type;
    slot->rights = rights;
    slot->badge = badge;
    slot->obj = obj;
    h = nos_handle_make(index, slot->gen);
    spin_unlock_irq(&table->lock);

    if (out_handle != NULL) {
        *out_handle = h;
    }
    return 0;
}

int nos_cap_alloc(struct nos_cap_table *table,
                  enum nos_cap_type type,
                  void *obj,
                  nos_cap_rights_t rights,
                  uint32_t badge,
                  nos_handle_t *out_handle)
{
    uint16_t i;
    uint16_t start_slot;
    struct nos_cap_slot *slot;
    nos_handle_t h;

    if (table == NULL || type == NOS_CAP_NONE || obj == NULL) {
        return -EINVAL;
    }

    start_slot = (NOS_CAP_DYNAMIC_START_SLOT < NOS_CAP_MAX_SLOTS)
                     ? (uint16_t)NOS_CAP_DYNAMIC_START_SLOT
                     : 1U;

    spin_lock_irq(&table->lock);
    for (i = start_slot; i < NOS_CAP_MAX_SLOTS; i++) {
        if (table->slots[i].type == NOS_CAP_NONE) {
            slot = &table->slots[i];
            goto found_slot;
        }
    }
    for (i = 1U; i < start_slot; i++) {
        if (table->slots[i].type == NOS_CAP_NONE) {
            slot = &table->slots[i];
            goto found_slot;
        }
    }
    spin_unlock_irq(&table->lock);
    return -ENOSPC;

found_slot:
    if (slot->gen == 0U) {
        slot->gen = 1U;
    }
    slot->type = (uint16_t)type;
    slot->rights = rights;
    slot->badge = badge;
    slot->obj = obj;
    h = nos_handle_make(i, slot->gen);
    spin_unlock_irq(&table->lock);

    if (out_handle != NULL) {
        *out_handle = h;
    }
    return 0;
}

int nos_cap_lookup(struct nos_cap_table *table,
                   nos_handle_t handle,
                   enum nos_cap_type expected_type,
                   nos_cap_rights_t required_rights,
                   void **out_obj,
                   uint32_t *out_badge)
{
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *slot;

    if (table == NULL || handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || idx >= NOS_CAP_MAX_SLOTS || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = &table->slots[idx];
    if (slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
        return -EBADF;
    }
    if (expected_type != NOS_CAP_NONE && slot->type != (uint16_t)expected_type) {
        spin_unlock_irq(&table->lock);
        return -EINVAL;
    }
    if ((slot->rights & required_rights) != required_rights) {
        spin_unlock_irq(&table->lock);
        return -EACCES;
    }
    if (out_obj != NULL) {
        *out_obj = slot->obj;
    }
    if (out_badge != NULL) {
        *out_badge = slot->badge;
    }
    spin_unlock_irq(&table->lock);

    return 0;
}

int nos_cap_derive(struct nos_cap_table *src_table,
                   nos_handle_t src_handle,
                   struct nos_cap_table *dst_table,
                   nos_cap_rights_t new_rights,
                   uint32_t new_badge,
                   nos_handle_t *out_handle)
{
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot snap;
    nos_cap_rights_t effective_rights;
    uint32_t effective_badge;

    if (src_table == NULL || dst_table == NULL || src_handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(src_handle);
    gen = nos_handle_gen(src_handle);
    if (idx == 0U || idx >= NOS_CAP_MAX_SLOTS || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&src_table->lock);
    snap = src_table->slots[idx];
    spin_unlock_irq(&src_table->lock);

    if (snap.type == NOS_CAP_NONE || snap.gen != gen) {
        return -EBADF;
    }

    /* Cross-table transfer requires NOS_CAP_RIGHT_GRANT */
    if (src_table != dst_table && !(snap.rights & NOS_CAP_RIGHT_GRANT)) {
        return -EACCES;
    }

    effective_rights = (new_rights != 0U) ? new_rights : snap.rights;
    /* Monotonic attenuation: cannot amplify rights beyond source capability */
    if ((effective_rights & ~snap.rights) != 0U) {
        return -EACCES;
    }

    /* Badge can only be set if source badge was 0 (unbadged master cap) */
    if (snap.badge != 0U && new_badge != 0U && new_badge != snap.badge) {
        return -EACCES;
    }
    effective_badge = (snap.badge != 0U) ? snap.badge : new_badge;

    return nos_cap_alloc(dst_table,
                         (enum nos_cap_type)snap.type,
                         snap.obj,
                         effective_rights,
                         effective_badge,
                         out_handle);
}

int nos_cap_revoke(struct nos_cap_table *table, nos_handle_t handle)
{
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *slot;

    if (table == NULL || handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || idx >= NOS_CAP_MAX_SLOTS || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = &table->slots[idx];
    if (slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
        return -EBADF;
    }
    slot->type = NOS_CAP_NONE;
    slot->gen = next_generation(slot->gen);
    slot->rights = 0U;
    slot->badge = 0U;
    slot->obj = NULL;
    spin_unlock_irq(&table->lock);

    return 0;
}

int nos_cap_inspect(struct nos_cap_table *table,
                    nos_handle_t handle,
                    uint32_t *out_type,
                    nos_cap_rights_t *out_rights)
{
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *slot;

    if (table == NULL || handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || idx >= NOS_CAP_MAX_SLOTS || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = &table->slots[idx];
    if (slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
        return -EBADF;
    }
    if (out_type != NULL) {
        *out_type = (uint32_t)slot->type;
    }
    if (out_rights != NULL) {
        *out_rights = slot->rights;
    }
    spin_unlock_irq(&table->lock);

    return 0;
}
