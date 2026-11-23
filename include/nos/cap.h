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

#ifdef CONFIG_NOS_CAP_CDT
#define NOS_CAP_CDT_MAX_SLOTS       4096U
#define NOS_CAP_EXT_SLOTS_PER_PAGE  64U
#define NOS_CAP_EXT_MAX_PAGES       ((NOS_CAP_CDT_MAX_SLOTS - NOS_CAP_MAX_SLOTS + \
                                      NOS_CAP_EXT_SLOTS_PER_PAGE - 1U) / \
                                     NOS_CAP_EXT_SLOTS_PER_PAGE)
#else
#define NOS_CAP_CDT_MAX_SLOTS       NOS_CAP_MAX_SLOTS
#endif

struct task_struct;
struct nos_cap_table;

/* 能力审计回调签名（在释放 CDT/能力表自旋锁后调用，避免锁序反转） */
typedef void (*nos_cap_audit_cb_t)(void *cell,
                                   uint16_t event_type,
                                   uint64_t obj_id,
                                   uint32_t badge,
                                   int32_t status,
                                   uint64_t arg0,
                                   uint64_t arg1);

/* 级联撤销在途排空回调签名（用于排空 RingIPC/AccelQueue 等在途请求） */
typedef void (*nos_cap_revoke_drain_cb_t)(uint16_t cap_type, void *obj);

struct nos_cap_slot {
    uint16_t         type;       /* enum nos_cap_type */
    uint16_t         gen;        /* 16-bit generation counter (never 0 when active) */
    nos_cap_rights_t rights;     /* NOS_CAP_RIGHT_* bitmask */
    uint32_t         badge;      /* Unforgeable token injected on IPC send */
#ifdef CONFIG_NOS_CAP_CDT
    uint16_t         slot_idx;   /* 物理槽位索引（用于 CDT 反向句柄合成） */
    uint16_t         cdt_flags;  /* 保留标志位 */
#endif
    void            *obj;        /* Kernel object pointer */
#ifdef CONFIG_NOS_CAP_CDT
    struct nos_cap_table *owner_table;      /* 所属 Capability 表 */
    struct nos_cap_slot  *cdt_parent;       /* CDT 父节点 */
    struct nos_cap_slot  *cdt_first_child;  /* CDT 首子节点 */
    struct nos_cap_slot  *cdt_next_sibling; /* CDT 下一兄弟节点 */
    struct nos_cap_slot  *cdt_prev_sibling; /* CDT 上一兄弟节点 */
#endif
};

struct nos_cap_table {
    spinlock_t          lock;
    struct nos_cap_slot slots[NOS_CAP_MAX_SLOTS];
#ifdef CONFIG_NOS_CAP_CDT
    struct nos_cap_slot **ext_pages;        /* 按需页式扩展二层槽位目录 */
    uint16_t             cap_capacity;      /* 当前已分配的总槽位数 (<= 4096) */
    uint16_t             ext_page_count;    /* 已分配扩展页数量 */
    void                *audit_cell;        /* 关联的 Agent Cell 审计对象指针 */
    uint64_t            *base_schema_masks; /* 首页槽位可选 schema_mask 约束数组 */
    uint64_t           **ext_schema_pages;  /* 扩展页槽位可选 schema_mask 目录 */
#endif
};

struct nos_cap_snapshot_entry {
    uint16_t         slot_idx;
    uint16_t         type;
    uint16_t         gen;
    nos_cap_rights_t rights;
    uint32_t         badge;
    void            *obj;
    uint64_t         schema_mask;
};

struct nos_cap_table_snapshot {
    struct nos_cap_table          *owner_table;
    uint32_t                       entry_count;
    struct nos_cap_snapshot_entry *entries;
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

int nos_cap_revoke_tree(struct nos_cap_table *table,
                        nos_handle_t handle,
                        uint32_t *out_revoked_count);

void nos_cap_register_audit_hook(nos_cap_audit_cb_t hook);
void nos_cap_register_revoke_drain_hook(nos_cap_revoke_drain_cb_t hook);
void nos_cap_table_bind_cell(struct nos_cap_table *table, void *cell);

int nos_cap_set_schema_mask(struct nos_cap_table *table,
                            nos_handle_t handle,
                            uint64_t schema_mask);
int nos_cap_get_schema_mask(struct nos_cap_table *table,
                            nos_handle_t handle,
                            uint64_t *out_mask);
int nos_cap_check_schema(struct nos_cap_table *table,
                         nos_handle_t handle,
                         uint64_t schema_id);

struct nos_cap_table_snapshot *nos_cap_table_snapshot(struct nos_cap_table *table);
int nos_cap_table_restore(struct nos_cap_table *table,
                          const struct nos_cap_table_snapshot *snap);
int nos_cap_table_restore_ex(struct nos_cap_table *table,
                             const struct nos_cap_table_snapshot *snap,
                             const void *preserve_snap_obj);
void nos_cap_table_snapshot_free(struct nos_cap_table_snapshot *snap);
int nos_cap_table_clone_for_fork(struct nos_cap_table *src_table,
                                 struct nos_cap_table *dst_table);

int nos_cap_inspect(struct nos_cap_table *table,
                    nos_handle_t handle,
                    uint32_t *out_type,
                    nos_cap_rights_t *out_rights);

#endif /* __NOS_CAP_H__ */
