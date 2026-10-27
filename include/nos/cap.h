/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Capability Table & Generation-Counted Handle Management
 */

#ifndef __NOS_CAP_H__
#define __NOS_CAP_H__

#include <uapi/nos/types.h>
#include <uapi/nos/cap.h>
#include <kernel/spinlock.h>

#ifndef CONFIG_NOS_CAP_MAX_SLOTS
#define CONFIG_NOS_CAP_MAX_SLOTS 16U
#endif

#define NOS_CAP_MAX_SLOTS CONFIG_NOS_CAP_MAX_SLOTS

struct task_struct;

struct nos_cap_slot {
    uint16_t         type;       /* enum nos_cap_type */
    uint16_t         gen;        /* 16-bit generation counter (never 0 when active) */
    nos_cap_rights_t rights;     /* NOS_CAP_RIGHT_* bitmask */
    uint32_t         badge;      /* Unforgeable token injected on IPC send */
    void            *obj;        /* Kernel object pointer */
};

struct nos_cap_table {
    spinlock_t          lock;
    struct nos_cap_slot slots[NOS_CAP_MAX_SLOTS];
};

void nos_cap_table_init(struct nos_cap_table *table);
void nos_cap_table_destroy(struct nos_cap_table *table);

int nos_cap_alloc(struct nos_cap_table *table,
                  enum nos_cap_type type,
                  void *obj,
                  nos_cap_rights_t rights,
                  uint32_t badge,
                  nos_handle_t *out_handle);

int nos_cap_alloc_at(struct nos_cap_table *table,
                     uint16_t index,
                     enum nos_cap_type type,
                     void *obj,
                     nos_cap_rights_t rights,
                     uint32_t badge,
                     nos_handle_t *out_handle);

int nos_cap_lookup(struct nos_cap_table *table,
                   nos_handle_t handle,
                   enum nos_cap_type expected_type,
                   nos_cap_rights_t required_rights,
                   void **out_obj,
                   uint32_t *out_badge);

int nos_cap_derive(struct nos_cap_table *src_table,
                   nos_handle_t src_handle,
                   struct nos_cap_table *dst_table,
                   nos_cap_rights_t new_rights,
                   uint32_t new_badge,
                   nos_handle_t *out_handle);

int nos_cap_revoke(struct nos_cap_table *table, nos_handle_t handle);

int nos_cap_inspect(struct nos_cap_table *table,
                    nos_handle_t handle,
                    uint32_t *out_type,
                    nos_cap_rights_t *out_rights);

#endif /* __NOS_CAP_H__ */
