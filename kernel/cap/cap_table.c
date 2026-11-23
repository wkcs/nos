/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Capability Table, Expandable Two-Level CNode & CDT Implementation
 */

#define pr_fmt(fmt) "[CAP]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/cap.h>
#include <nos/object.h>
#include <uapi/nos/abi.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <string.h>

#ifdef CONFIG_NOS_CAP_CDT
#include <kernel/mm.h>
#include <uapi/nos/ai.h>
#endif

#define NOS_CAP_DYNAMIC_START_SLOT (NOS_CAP_SLOT_VFS_EP + 1U)

#ifdef CONFIG_NOS_CAP_CDT
/* 全局 CDT 拓扑保护锁：加锁顺序严格为 g_cdt_lock -> table->lock（双表按地址升序） */
static spinlock_t g_cdt_lock;
static nos_cap_audit_cb_t g_cap_audit_hook;
#define CAP_REVOKE_DRAIN_MAX_HOOKS 4U
static nos_cap_revoke_drain_cb_t g_cap_revoke_drain_hooks[CAP_REVOKE_DRAIN_MAX_HOOKS];

struct cap_drain_item {
    uint16_t type;
    void    *obj;
};
#endif

static uint16_t next_generation(uint16_t current_gen)
{
    uint16_t next = (uint16_t)(current_gen + 1U);
    return (next == 0U) ? 1U : next;
}

static bool cap_type_has_object_header(uint16_t type)
{
    return (type == NOS_CAP_ENDPOINT ||
            type == NOS_CAP_NOTIFICATION ||
            type == NOS_CAP_VMO ||
            type == NOS_CAP_CELL ||
            type == NOS_CAP_RING ||
            type == NOS_CAP_ACCEL ||
            type == NOS_CAP_SNAPSHOT);
}

static uint16_t cap_table_capacity_locked(const struct nos_cap_table *table)
{
#ifdef CONFIG_NOS_CAP_CDT
    return (table->cap_capacity != 0U) ? table->cap_capacity : (uint16_t)NOS_CAP_MAX_SLOTS;
#else
    (void)table;
    return (uint16_t)NOS_CAP_MAX_SLOTS;
#endif
}

static struct nos_cap_slot *cap_slot_ptr_locked(struct nos_cap_table *table, uint16_t idx)
{
    if (idx < NOS_CAP_MAX_SLOTS) {
        return &table->slots[idx];
    }
#ifdef CONFIG_NOS_CAP_CDT
    if (idx < cap_table_capacity_locked(table) && table->ext_pages != NULL) {
        uint32_t ext_idx = (uint32_t)idx - NOS_CAP_MAX_SLOTS;
        uint32_t page_idx = ext_idx / NOS_CAP_EXT_SLOTS_PER_PAGE;
        uint32_t in_page = ext_idx % NOS_CAP_EXT_SLOTS_PER_PAGE;

        if (page_idx < table->ext_page_count && table->ext_pages[page_idx] != NULL) {
            return &table->ext_pages[page_idx][in_page];
        }
    }
#endif
    return NULL;
}

static void cap_init_slot(struct nos_cap_slot *slot,
                          struct nos_cap_table *owner,
                          uint16_t idx,
                          uint16_t gen)
{
    slot->type = NOS_CAP_NONE;
    slot->gen = (gen == 0U) ? 1U : gen;
    slot->rights = 0U;
    slot->badge = 0U;
    slot->obj = NULL;
#ifdef CONFIG_NOS_CAP_CDT
    slot->slot_idx = idx;
    slot->cdt_flags = 0U;
    slot->owner_table = owner;
    slot->cdt_parent = NULL;
    slot->cdt_first_child = NULL;
    slot->cdt_next_sibling = NULL;
    slot->cdt_prev_sibling = NULL;
#else
    (void)owner;
    (void)idx;
#endif
}

#ifdef CONFIG_NOS_CAP_CDT
static uint64_t *cap_schema_ptr_locked(struct nos_cap_table *table, uint16_t idx)
{
    if (table == NULL) {
        return NULL;
    }
    if (idx < NOS_CAP_MAX_SLOTS) {
        return (table->base_schema_masks != NULL) ? &table->base_schema_masks[idx] : NULL;
    }
    if (idx < cap_table_capacity_locked(table) && table->ext_schema_pages != NULL) {
        uint32_t ext_idx = (uint32_t)idx - NOS_CAP_MAX_SLOTS;
        uint32_t page_idx = ext_idx / NOS_CAP_EXT_SLOTS_PER_PAGE;
        uint32_t in_page = ext_idx % NOS_CAP_EXT_SLOTS_PER_PAGE;
        if (page_idx < NOS_CAP_EXT_MAX_PAGES && table->ext_schema_pages[page_idx] != NULL) {
            return &table->ext_schema_pages[page_idx][in_page];
        }
    }
    return NULL;
}

static uint64_t cap_schema_get_locked(struct nos_cap_table *table, uint16_t idx)
{
    uint64_t *p = cap_schema_ptr_locked(table, idx);
    return (p != NULL) ? *p : 0ULL;
}

static int cap_ensure_schema_slot(struct nos_cap_table *table, uint16_t idx)
{
    uint64_t *new_base = NULL;
    uint64_t **new_dir = NULL;
    uint64_t *new_page = NULL;

    if (table == NULL || idx >= NOS_CAP_CDT_MAX_SLOTS) {
        return -EINVAL;
    }

    if (idx < NOS_CAP_MAX_SLOTS) {
        spin_lock_irq(&table->lock);
        if (table->base_schema_masks != NULL) {
            spin_unlock_irq(&table->lock);
            return 0;
        }
        spin_unlock_irq(&table->lock);

        new_base = kzalloc((uint32_t)(sizeof(uint64_t) * NOS_CAP_MAX_SLOTS), GFP_KERNEL);
        if (new_base == NULL) {
            return -ENOMEM;
        }
        spin_lock_irq(&table->lock);
        if (table->base_schema_masks == NULL) {
            table->base_schema_masks = new_base;
            new_base = NULL;
        }
        spin_unlock_irq(&table->lock);
        if (new_base != NULL) {
            kfree(new_base);
        }
        return 0;
    }

    {
        uint32_t ext_idx = (uint32_t)idx - NOS_CAP_MAX_SLOTS;
        uint32_t page_idx = ext_idx / NOS_CAP_EXT_SLOTS_PER_PAGE;
        bool need_dir;

        if (page_idx >= NOS_CAP_EXT_MAX_PAGES) {
            return -EINVAL;
        }
        spin_lock_irq(&table->lock);
        if (table->ext_schema_pages != NULL && table->ext_schema_pages[page_idx] != NULL) {
            spin_unlock_irq(&table->lock);
            return 0;
        }
        need_dir = (table->ext_schema_pages == NULL);
        spin_unlock_irq(&table->lock);

        if (need_dir) {
            new_dir = kzalloc((uint32_t)(sizeof(uint64_t *) * NOS_CAP_EXT_MAX_PAGES), GFP_KERNEL);
            if (new_dir == NULL) {
                return -ENOMEM;
            }
        }
        new_page = kzalloc((uint32_t)(sizeof(uint64_t) * NOS_CAP_EXT_SLOTS_PER_PAGE), GFP_KERNEL);
        if (new_page == NULL) {
            if (new_dir != NULL) {
                kfree(new_dir);
            }
            return -ENOMEM;
        }

        spin_lock_irq(&table->lock);
        if (table->ext_schema_pages == NULL && new_dir != NULL) {
            table->ext_schema_pages = new_dir;
            new_dir = NULL;
        }
        if (table->ext_schema_pages != NULL && table->ext_schema_pages[page_idx] == NULL) {
            table->ext_schema_pages[page_idx] = new_page;
            new_page = NULL;
        }
        spin_unlock_irq(&table->lock);

        if (new_dir != NULL) {
            kfree(new_dir);
        }
        if (new_page != NULL) {
            kfree(new_page);
        }
    }
    return 0;
}
#endif

static void cap_clear_slot_preserve_owner(struct nos_cap_slot *slot)
{
    slot->type = NOS_CAP_NONE;
    slot->gen = next_generation(slot->gen);
    slot->rights = 0U;
    slot->badge = 0U;
    slot->obj = NULL;
#ifdef CONFIG_NOS_CAP_CDT
    if (slot->owner_table != NULL) {
        uint64_t *sp = cap_schema_ptr_locked(slot->owner_table, slot->slot_idx);
        if (sp != NULL) {
            *sp = 0ULL;
        }
    }
    slot->cdt_flags = 0U;
    slot->cdt_parent = NULL;
    slot->cdt_first_child = NULL;
    slot->cdt_next_sibling = NULL;
    slot->cdt_prev_sibling = NULL;
#endif
}

static struct nos_cap_slot *cap_find_free_slot_locked(struct nos_cap_table *table)
{
    uint16_t cap = cap_table_capacity_locked(table);
    uint16_t start_slot = (NOS_CAP_DYNAMIC_START_SLOT < NOS_CAP_MAX_SLOTS)
                              ? (uint16_t)NOS_CAP_DYNAMIC_START_SLOT
                              : 1U;
    uint16_t i;

    for (i = start_slot; i < cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, i);
        if (slot != NULL && slot->type == NOS_CAP_NONE) {
            return slot;
        }
    }
    for (i = 1U; i < start_slot && i < cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, i);
        if (slot != NULL && slot->type == NOS_CAP_NONE) {
            return slot;
        }
    }
    return NULL;
}

static uint16_t cap_slot_handle_index(const struct nos_cap_table *table,
                                      const struct nos_cap_slot *slot)
{
#ifdef CONFIG_NOS_CAP_CDT
    (void)table;
    return slot->slot_idx;
#else
    return (uint16_t)(slot - &table->slots[0]);
#endif
}

#ifdef CONFIG_NOS_CAP_CDT
static void cap_lock_two_tables(struct nos_cap_table *a, struct nos_cap_table *b)
{
    if (a == b) {
        spin_lock_irq(&a->lock);
    } else if ((uintptr_t)a < (uintptr_t)b) {
        spin_lock_irq(&a->lock);
        spin_lock_irq(&b->lock);
    } else {
        spin_lock_irq(&b->lock);
        spin_lock_irq(&a->lock);
    }
}

static void cap_unlock_two_tables(struct nos_cap_table *a, struct nos_cap_table *b)
{
    if (a == b) {
        spin_unlock_irq(&a->lock);
    } else if ((uintptr_t)a < (uintptr_t)b) {
        spin_unlock_irq(&b->lock);
        spin_unlock_irq(&a->lock);
    } else {
        spin_unlock_irq(&a->lock);
        spin_unlock_irq(&b->lock);
    }
}

/* 在 g_cdt_lock 保护下将 child 挂入 parent 的子节点链表头部 */
static void cap_cdt_attach_locked(struct nos_cap_slot *parent, struct nos_cap_slot *child)
{
    child->cdt_parent = parent;
    child->cdt_prev_sibling = NULL;
    child->cdt_next_sibling = parent->cdt_first_child;
    if (parent->cdt_first_child != NULL) {
        parent->cdt_first_child->cdt_prev_sibling = child;
    }
    parent->cdt_first_child = child;
}

/* 在 g_cdt_lock 保护下将 node 从其父节点的孩子链表摘除 */
static void cap_cdt_unlink_from_parent_locked(struct nos_cap_slot *node)
{
    struct nos_cap_slot *parent = node->cdt_parent;
    struct nos_cap_slot *prev = node->cdt_prev_sibling;
    struct nos_cap_slot *next = node->cdt_next_sibling;

    if (prev != NULL) {
        prev->cdt_next_sibling = next;
    } else if (parent != NULL && parent->cdt_first_child == node) {
        parent->cdt_first_child = next;
    }

    if (next != NULL) {
        next->cdt_prev_sibling = prev;
    }

    node->cdt_parent = NULL;
    node->cdt_prev_sibling = NULL;
    node->cdt_next_sibling = NULL;
}

/*
 * 单槽摘链并安全提升子节点（Reparent）：
 * 在单槽撤销或所属表即将析构销毁时，将仍存活的子节点挂接回原父节点，
 * 避免其它能力表中残留指向已失效/待释放槽位的悬空 CDT 指针。
 */
static void cap_cdt_detach_reparent_locked(struct nos_cap_slot *node)
{
    struct nos_cap_slot *parent = node->cdt_parent;
    struct nos_cap_slot *child = node->cdt_first_child;

    node->cdt_first_child = NULL;
    cap_cdt_unlink_from_parent_locked(node);

    while (child != NULL) {
        struct nos_cap_slot *next = child->cdt_next_sibling;

        child->cdt_prev_sibling = NULL;
        child->cdt_next_sibling = NULL;
        if (parent != NULL) {
            cap_cdt_attach_locked(parent, child);
        } else {
            child->cdt_parent = NULL;
        }
        child = next;
    }
}

/*
 * 在未持有自旋锁时为 CNode 预扩展一页（最多 64 个槽位）：
 * 通过锁外分配、锁内原子挂载的方式彻底规避 spin_lock_irq 下调用分配器的死锁风险。
 */
static int cap_pre_expand_one_page(struct nos_cap_table *table, bool force_if_below)
{
    struct nos_cap_slot **new_dir = NULL;
    struct nos_cap_slot *new_page = NULL;
    bool need_dir;
    uint16_t cap;

    spin_lock_irq(&table->lock);
    if (table->cap_capacity == 0U) {
        table->cap_capacity = (uint16_t)NOS_CAP_MAX_SLOTS;
    }
    cap = cap_table_capacity_locked(table);
    if (!force_if_below && cap_find_free_slot_locked(table) != NULL) {
        spin_unlock_irq(&table->lock);
        return 0;
    }
    if (cap >= NOS_CAP_CDT_MAX_SLOTS) {
        spin_unlock_irq(&table->lock);
        return -ENOSPC;
    }
    need_dir = (table->ext_pages == NULL);
    spin_unlock_irq(&table->lock);

    if (need_dir) {
        new_dir = kzalloc((uint32_t)(sizeof(struct nos_cap_slot *) * NOS_CAP_EXT_MAX_PAGES),
                          GFP_KERNEL);
        if (new_dir == NULL) {
            return -ENOMEM;
        }
    }

    new_page = kzalloc((uint32_t)(sizeof(struct nos_cap_slot) * NOS_CAP_EXT_SLOTS_PER_PAGE),
                       GFP_KERNEL);
    if (new_page == NULL) {
        if (new_dir != NULL) {
            kfree(new_dir);
        }
        return -ENOMEM;
    }

    spin_lock_irq(&table->lock);
    if (table->ext_pages == NULL && new_dir != NULL) {
        table->ext_pages = new_dir;
        new_dir = NULL;
    }
    if (table->cap_capacity < NOS_CAP_CDT_MAX_SLOTS &&
        table->ext_page_count < NOS_CAP_EXT_MAX_PAGES &&
        (force_if_below || cap_find_free_slot_locked(table) == NULL)) {
        uint16_t pidx = table->ext_page_count;
        uint16_t base_idx = (uint16_t)(NOS_CAP_MAX_SLOTS +
                                       pidx * NOS_CAP_EXT_SLOTS_PER_PAGE);
        uint16_t remain = (uint16_t)(NOS_CAP_CDT_MAX_SLOTS - base_idx);
        uint16_t count = (remain < NOS_CAP_EXT_SLOTS_PER_PAGE)
                             ? remain
                             : (uint16_t)NOS_CAP_EXT_SLOTS_PER_PAGE;
        uint16_t j;

        for (j = 0; j < count; j++) {
            cap_init_slot(&new_page[j], table, (uint16_t)(base_idx + j), 1U);
        }
        table->ext_pages[pidx] = new_page;
        table->ext_page_count++;
        table->cap_capacity = (uint16_t)(table->cap_capacity + count);
        new_page = NULL;
    }
    spin_unlock_irq(&table->lock);

    if (new_dir != NULL) {
        kfree(new_dir);
    }
    if (new_page != NULL) {
        kfree(new_page);
    }
    return 0;
}

static void cap_emit_audit(void *cell,
                           uint16_t event_type,
                           void *obj,
                           uint32_t badge,
                           int32_t status,
                           uint64_t arg0,
                           uint64_t arg1)
{
    nos_cap_audit_cb_t hook = __atomic_load_n(&g_cap_audit_hook, __ATOMIC_ACQUIRE);

    if (hook != NULL && cell != NULL) {
        hook(cell, event_type, (uint64_t)(uintptr_t)obj, badge, status, arg0, arg1);
    }
}
#endif

void nos_cap_register_audit_hook(nos_cap_audit_cb_t hook)
{
#ifdef CONFIG_NOS_CAP_CDT
    __atomic_store_n(&g_cap_audit_hook, hook, __ATOMIC_RELEASE);
#else
    (void)hook;
#endif
}

void nos_cap_register_revoke_drain_hook(nos_cap_revoke_drain_cb_t hook)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint32_t i;

    if (hook == NULL) {
        for (i = 0U; i < CAP_REVOKE_DRAIN_MAX_HOOKS; i++) {
            __atomic_store_n(&g_cap_revoke_drain_hooks[i], NULL, __ATOMIC_RELEASE);
        }
        return;
    }

    for (i = 0U; i < CAP_REVOKE_DRAIN_MAX_HOOKS; i++) {
        nos_cap_revoke_drain_cb_t cur =
            __atomic_load_n(&g_cap_revoke_drain_hooks[i], __ATOMIC_ACQUIRE);
        if (cur == hook) {
            return;
        }
        if (cur == NULL) {
            __atomic_store_n(&g_cap_revoke_drain_hooks[i], hook, __ATOMIC_RELEASE);
            return;
        }
    }
#else
    (void)hook;
#endif
}

void nos_cap_table_bind_cell(struct nos_cap_table *table, void *cell)
{
#ifdef CONFIG_NOS_CAP_CDT
    if (table == NULL) {
        return;
    }
    spin_lock_irq(&table->lock);
    table->audit_cell = cell;
    spin_unlock_irq(&table->lock);
#else
    (void)table;
    (void)cell;
#endif
}

void nos_cap_table_init(struct nos_cap_table *table)
{
    uint32_t i;

    if (table == NULL) {
        return;
    }

    spin_lock_init(&table->lock);
    for (i = 0; i < NOS_CAP_MAX_SLOTS; i++) {
        cap_init_slot(&table->slots[i], table, (uint16_t)i, 1U);
    }
#ifdef CONFIG_NOS_CAP_CDT
    table->ext_pages = NULL;
    table->cap_capacity = (uint16_t)NOS_CAP_MAX_SLOTS;
    table->ext_page_count = 0U;
    table->audit_cell = NULL;
    table->base_schema_masks = NULL;
    table->ext_schema_pages = NULL;
#endif
}

void nos_cap_table_destroy(struct nos_cap_table *table)
{
    uint16_t i;
    uint16_t cap;
#ifdef CONFIG_NOS_CAP_CDT
    struct nos_cap_slot **ext_pages = NULL;
    uint16_t ext_page_count = 0U;
    uint64_t *base_schemas = NULL;
    uint64_t **ext_schemas = NULL;
    uint16_t p;
#endif

    if (table == NULL) {
        return;
    }

#ifdef CONFIG_NOS_CAP_CDT
    spin_lock_irq(&g_cdt_lock);
#endif
    spin_lock_irq(&table->lock);
    cap = cap_table_capacity_locked(table);
    for (i = 0; i < cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, i);
        if (slot == NULL) {
            continue;
        }
#ifdef CONFIG_NOS_CAP_CDT
        cap_cdt_detach_reparent_locked(slot);
#endif
        cap_clear_slot_preserve_owner(slot);
    }
#ifdef CONFIG_NOS_CAP_CDT
    ext_pages = table->ext_pages;
    ext_page_count = table->ext_page_count;
    base_schemas = table->base_schema_masks;
    ext_schemas = table->ext_schema_pages;
    table->ext_pages = NULL;
    table->ext_page_count = 0U;
    table->cap_capacity = (uint16_t)NOS_CAP_MAX_SLOTS;
    table->audit_cell = NULL;
    table->base_schema_masks = NULL;
    table->ext_schema_pages = NULL;
#endif
    spin_unlock_irq(&table->lock);
#ifdef CONFIG_NOS_CAP_CDT
    spin_unlock_irq(&g_cdt_lock);

    if (ext_pages != NULL) {
        for (p = 0; p < ext_page_count; p++) {
            if (ext_pages[p] != NULL) {
                kfree(ext_pages[p]);
            }
        }
        kfree(ext_pages);
    }
    if (base_schemas != NULL) {
        kfree(base_schemas);
    }
    if (ext_schemas != NULL) {
        for (p = 0; p < NOS_CAP_EXT_MAX_PAGES; p++) {
            if (ext_schemas[p] != NULL) {
                kfree(ext_schemas[p]);
            }
        }
        kfree(ext_schemas);
    }
#endif
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
    if (index == 0U || index >= NOS_CAP_CDT_MAX_SLOTS) {
        return -EINVAL;
    }

#ifdef CONFIG_NOS_CAP_CDT
    for (;;) {
        uint16_t cur_cap = __atomic_load_n(&table->cap_capacity, __ATOMIC_ACQUIRE);
        if (cur_cap == 0U) {
            cur_cap = (uint16_t)NOS_CAP_MAX_SLOTS;
        }
        if (cur_cap > index) {
            break;
        }
        {
            int rc = cap_pre_expand_one_page(table, true);
            if (rc != 0) {
                return rc;
            }
        }
    }
#endif

    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, index);
    if (slot == NULL) {
        spin_unlock_irq(&table->lock);
        return -EINVAL;
    }
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
    struct nos_cap_slot *slot = NULL;
    nos_handle_t h = NOS_HANDLE_INVALID;
    uint16_t idx;
#ifdef CONFIG_NOS_CAP_CDT
    uint32_t retry;
#endif

    if (table == NULL || type == NOS_CAP_NONE || obj == NULL) {
        return -EINVAL;
    }

#ifdef CONFIG_NOS_CAP_CDT
    for (retry = 0U; retry <= NOS_CAP_EXT_MAX_PAGES; retry++) {
        spin_lock_irq(&table->lock);
        slot = cap_find_free_slot_locked(table);
        if (slot != NULL) {
            if (slot->gen == 0U) {
                slot->gen = 1U;
            }
            slot->type = (uint16_t)type;
            slot->rights = rights;
            slot->badge = badge;
            slot->obj = obj;
            idx = cap_slot_handle_index(table, slot);
            h = nos_handle_make(idx, slot->gen);
            spin_unlock_irq(&table->lock);
            if (out_handle != NULL) {
                *out_handle = h;
            }
            return 0;
        }
        if (table->cap_capacity >= NOS_CAP_CDT_MAX_SLOTS) {
            spin_unlock_irq(&table->lock);
            return -ENOSPC;
        }
        spin_unlock_irq(&table->lock);

        {
            int rc = cap_pre_expand_one_page(table, false);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return -ENOSPC;
#else
    spin_lock_irq(&table->lock);
    slot = cap_find_free_slot_locked(table);
    if (slot == NULL) {
        spin_unlock_irq(&table->lock);
        return -ENOSPC;
    }

    if (slot->gen == 0U) {
        slot->gen = 1U;
    }
    slot->type = (uint16_t)type;
    slot->rights = rights;
    slot->badge = badge;
    slot->obj = obj;
    idx = cap_slot_handle_index(table, slot);
    h = nos_handle_make(idx, slot->gen);
    spin_unlock_irq(&table->lock);

    if (out_handle != NULL) {
        *out_handle = h;
    }
    return 0;
#endif
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
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, idx);
    if (slot == NULL || slot->type == NOS_CAP_NONE || slot->gen != gen) {
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

    /*
     * Capability 查找与对象引用固定 (Pinned Lookup):
     * 在 Capability 表锁内同步递增对象的原子引用计数，
     * 确保后续系统调用或在途操作期间对象不被并发释放。
     * 若对象已处于退休或析构状态 (retired 或 refcount 归零)，返回错误。
     */
    if (out_obj != NULL && slot->obj != NULL && cap_type_has_object_header(slot->type)) {
        struct nos_object *obj = (struct nos_object *)slot->obj;
        if (obj->retired || !nos_object_get(obj)) {
            spin_unlock_irq(&table->lock);
            return -ESTALE;
        }
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
#ifdef CONFIG_NOS_CAP_CDT
    uint32_t retry;

    if (src_table == NULL || dst_table == NULL || src_handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(src_handle);
    gen = nos_handle_gen(src_handle);
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    for (retry = 0U; retry <= NOS_CAP_EXT_MAX_PAGES; retry++) {
        struct nos_cap_slot *src_slot;
        struct nos_cap_slot *dst_slot;
        nos_cap_rights_t effective_rights;
        uint32_t effective_badge;
        nos_handle_t h;
        void *audit_cell;
        void *obj_ptr;
        uint64_t src_mask;
        uint64_t *dst_mask_ptr = NULL;

        spin_lock_irq(&g_cdt_lock);
        cap_lock_two_tables(src_table, dst_table);

        src_slot = cap_slot_ptr_locked(src_table, idx);
        if (src_slot == NULL || src_slot->type == NOS_CAP_NONE || src_slot->gen != gen) {
            cap_unlock_two_tables(src_table, dst_table);
            spin_unlock_irq(&g_cdt_lock);
            return -EBADF;
        }

        /* 跨表能力转移必须显式拥有 GRANT 权限 */
        if (src_table != dst_table && !(src_slot->rights & NOS_CAP_RIGHT_GRANT)) {
            cap_unlock_two_tables(src_table, dst_table);
            spin_unlock_irq(&g_cdt_lock);
            return -EACCES;
        }

        effective_rights = (new_rights != 0U) ? new_rights : src_slot->rights;
        /* 单调衰减铁律：派生能力权限严禁超越父能力 */
        if ((effective_rights & ~src_slot->rights) != 0U) {
            cap_unlock_two_tables(src_table, dst_table);
            spin_unlock_irq(&g_cdt_lock);
            return -EACCES;
        }

        /* 已绑定非零 Badge 的能力严禁再改写为其它徽章 */
        if (src_slot->badge != 0U && new_badge != 0U && new_badge != src_slot->badge) {
            cap_unlock_two_tables(src_table, dst_table);
            spin_unlock_irq(&g_cdt_lock);
            return -EACCES;
        }
        effective_badge = (src_slot->badge != 0U) ? src_slot->badge : new_badge;
        src_mask = cap_schema_get_locked(src_table, idx);

        dst_slot = cap_find_free_slot_locked(dst_table);
        if (dst_slot == NULL) {
            bool can_expand = (dst_table->cap_capacity < NOS_CAP_CDT_MAX_SLOTS);
            cap_unlock_two_tables(src_table, dst_table);
            spin_unlock_irq(&g_cdt_lock);
            if (!can_expand) {
                return -ENOSPC;
            }
            {
                int rc = cap_pre_expand_one_page(dst_table, false);
                if (rc != 0) {
                    return rc;
                }
            }
            continue;
        }

        if (src_mask != 0ULL) {
            dst_mask_ptr = cap_schema_ptr_locked(dst_table, dst_slot->slot_idx);
            if (dst_mask_ptr == NULL) {
                uint16_t need_idx = dst_slot->slot_idx;
                cap_unlock_two_tables(src_table, dst_table);
                spin_unlock_irq(&g_cdt_lock);
                {
                    int rc = cap_ensure_schema_slot(dst_table, need_idx);
                    if (rc != 0) {
                        return rc;
                    }
                }
                continue;
            }
        }

        if (dst_slot->gen == 0U) {
            dst_slot->gen = 1U;
        }
        dst_slot->type = src_slot->type;
        dst_slot->rights = effective_rights;
        dst_slot->badge = effective_badge;
        dst_slot->obj = src_slot->obj;
        if (dst_mask_ptr != NULL) {
            *dst_mask_ptr = src_mask;
        } else {
            uint64_t *clr = cap_schema_ptr_locked(dst_table, dst_slot->slot_idx);
            if (clr != NULL) {
                *clr = 0ULL;
            }
        }
        cap_cdt_attach_locked(src_slot, dst_slot);

        h = nos_handle_make(cap_slot_handle_index(dst_table, dst_slot), dst_slot->gen);
        audit_cell = (dst_table->audit_cell != NULL) ? dst_table->audit_cell : src_table->audit_cell;
        obj_ptr = dst_slot->obj;

        cap_unlock_two_tables(src_table, dst_table);
        spin_unlock_irq(&g_cdt_lock);

        cap_emit_audit(audit_cell,
                       NOS_AUDIT_CAP_DERIVE,
                       obj_ptr,
                       effective_badge,
                       0,
                       (uint64_t)src_handle,
                       (uint64_t)h);

        if (out_handle != NULL) {
            *out_handle = h;
        }
        return 0;
    }

    return -ENOSPC;
#else
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
#endif
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
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

#ifdef CONFIG_NOS_CAP_CDT
    spin_lock_irq(&g_cdt_lock);
#endif
    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, idx);
    if (slot == NULL || slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
#ifdef CONFIG_NOS_CAP_CDT
        spin_unlock_irq(&g_cdt_lock);
#endif
        return -EBADF;
    }
#ifdef CONFIG_NOS_CAP_CDT
    cap_cdt_detach_reparent_locked(slot);
#endif
    cap_clear_slot_preserve_owner(slot);
    spin_unlock_irq(&table->lock);
#ifdef CONFIG_NOS_CAP_CDT
    spin_unlock_irq(&g_cdt_lock);
#endif

    return 0;
}

int nos_cap_revoke_tree(struct nos_cap_table *table,
                        nos_handle_t handle,
                        uint32_t *out_revoked_count)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *root;
    struct nos_cap_slot *cur;
    uint32_t revoked = 0U;
    void *audit_cell = NULL;
    void *root_obj = NULL;
    uint32_t root_badge = 0U;
    nos_cap_revoke_drain_cb_t drain_hooks[CAP_REVOKE_DRAIN_MAX_HOOKS];
    bool has_drain_hook = false;
    struct cap_drain_item drain_items[16];
    uint32_t drain_cnt = 0U;

    if (table == NULL || handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }

    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    for (uint32_t h_i = 0U; h_i < CAP_REVOKE_DRAIN_MAX_HOOKS; h_i++) {
        drain_hooks[h_i] = __atomic_load_n(&g_cap_revoke_drain_hooks[h_i], __ATOMIC_ACQUIRE);
        if (drain_hooks[h_i] != NULL) {
            has_drain_hook = true;
        }
    }

    spin_lock_irq(&g_cdt_lock);
    spin_lock_irq(&table->lock);
    root = cap_slot_ptr_locked(table, idx);
    if (root == NULL || root->type == NOS_CAP_NONE || root->gen != gen) {
        spin_unlock_irq(&table->lock);
        spin_unlock_irq(&g_cdt_lock);
        return -EBADF;
    }

    /* 先将整棵子树从其外部父节点摘断并保存根审计元数据 */
    audit_cell = table->audit_cell;
    root_obj = root->obj;
    root_badge = root->badge;
    cap_cdt_unlink_from_parent_locked(root);
    spin_unlock_irq(&table->lock);

    /*
     * 基于嵌入式父子链指针的原地无栈后序 DFS 级联撤销：
     * 栈空间严格为 O(1)，可安全吊销任意深度（最高 4096 层）且横跨多个 CNode 的派生树。
     */
    cur = root;
    while (cur != NULL) {
        if (cur->cdt_first_child != NULL) {
            struct nos_cap_slot *child = cur->cdt_first_child;

            cur->cdt_first_child = child->cdt_next_sibling;
            child->cdt_prev_sibling = NULL;
            child->cdt_next_sibling = NULL;
            /* 保留 child->cdt_parent == cur 作为非递归回溯栈链 */
            cur = child;
            continue;
        }

        {
            struct nos_cap_slot *parent = cur->cdt_parent;
            struct nos_cap_table *cur_tbl = cur->owner_table;

            cur->cdt_parent = NULL;
            cur->cdt_first_child = NULL;
            cur->cdt_next_sibling = NULL;
            cur->cdt_prev_sibling = NULL;

            if (cur_tbl != NULL) {
                spin_lock_irq(&cur_tbl->lock);
                if (cur->type != NOS_CAP_NONE) {
                    if (audit_cell == NULL && cur_tbl->audit_cell != NULL) {
                        audit_cell = cur_tbl->audit_cell;
                    }
                    if (has_drain_hook &&
                        (cur->type == NOS_CAP_RING || cur->type == NOS_CAP_ACCEL) &&
                        cur->obj != NULL && drain_cnt < 16U) {
                        bool dup = false;
                        for (uint32_t d = 0U; d < drain_cnt; d++) {
                            if (drain_items[d].type == cur->type &&
                                drain_items[d].obj == cur->obj) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup && nos_object_get((struct nos_object *)cur->obj)) {
                            drain_items[drain_cnt].type = cur->type;
                            drain_items[drain_cnt].obj = cur->obj;
                            drain_cnt++;
                        }
                    }
                    cap_clear_slot_preserve_owner(cur);
                    revoked++;
                }
                spin_unlock_irq(&cur_tbl->lock);
            }
            cur = parent;
        }
    }

    spin_unlock_irq(&g_cdt_lock);

    for (uint32_t d = 0U; d < drain_cnt; d++) {
        for (uint32_t h_i = 0U; h_i < CAP_REVOKE_DRAIN_MAX_HOOKS; h_i++) {
            if (drain_hooks[h_i] != NULL) {
                drain_hooks[h_i](drain_items[d].type, drain_items[d].obj);
            }
        }
        (void)nos_object_put((struct nos_object *)drain_items[d].obj);
    }

    cap_emit_audit(audit_cell,
                   NOS_AUDIT_CAP_REVOKE,
                   root_obj,
                   root_badge,
                   0,
                   (uint64_t)handle,
                   (uint64_t)revoked);

    if (out_revoked_count != NULL) {
        *out_revoked_count = revoked;
    }
    return 0;
#else
    int rc = nos_cap_revoke(table, handle);
    if (rc == 0 && out_revoked_count != NULL) {
        *out_revoked_count = 1U;
    }
    return rc;
#endif
}

int nos_cap_set_schema_mask(struct nos_cap_table *table,
                            nos_handle_t handle,
                            uint64_t schema_mask)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *slot;
    uint64_t *mp;
    uint64_t old_mask;
    int rc;

    if (table == NULL || handle == NOS_HANDLE_INVALID) {
        return -EBADF;
    }
    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    if (schema_mask != 0ULL) {
        rc = cap_ensure_schema_slot(table, idx);
        if (rc != 0) {
            return rc;
        }
    }

    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, idx);
    if (slot == NULL || slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
        return -EBADF;
    }
    mp = cap_schema_ptr_locked(table, idx);
    old_mask = (mp != NULL) ? *mp : 0ULL;
    /* 若已有非零约束则只允许单调收紧，禁止放宽 */
    if (old_mask != 0ULL && (schema_mask == 0ULL || (schema_mask & ~old_mask) != 0ULL)) {
        spin_unlock_irq(&table->lock);
        return -EACCES;
    }
    if (mp != NULL) {
        *mp = schema_mask;
    }
    spin_unlock_irq(&table->lock);
    return 0;
#else
    (void)table;
    (void)handle;
    (void)schema_mask;
    return -ENOSYS;
#endif
}

int nos_cap_get_schema_mask(struct nos_cap_table *table,
                            nos_handle_t handle,
                            uint64_t *out_mask)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint16_t idx;
    uint16_t gen;
    struct nos_cap_slot *slot;

    if (table == NULL || handle == NOS_HANDLE_INVALID || out_mask == NULL) {
        return -EINVAL;
    }
    idx = nos_handle_index(handle);
    gen = nos_handle_gen(handle);
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, idx);
    if (slot == NULL || slot->type == NOS_CAP_NONE || slot->gen != gen) {
        spin_unlock_irq(&table->lock);
        return -EBADF;
    }
    *out_mask = cap_schema_get_locked(table, idx);
    spin_unlock_irq(&table->lock);
    return 0;
#else
    (void)table;
    (void)handle;
    (void)out_mask;
    return -ENOSYS;
#endif
}

int nos_cap_check_schema(struct nos_cap_table *table,
                         nos_handle_t handle,
                         uint64_t schema_id)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint64_t mask = 0ULL;
    int rc = nos_cap_get_schema_mask(table, handle, &mask);
    if (rc < 0) {
        return rc;
    }
    if (mask == 0ULL) {
        return 0;
    }
    if (schema_id == 0ULL || (schema_id & mask) != schema_id) {
        return -EACCES;
    }
    return 0;
#else
    (void)table;
    (void)handle;
    (void)schema_id;
    return 0;
#endif
}

struct nos_cap_table_snapshot *nos_cap_table_snapshot(struct nos_cap_table *table)
{
#ifdef CONFIG_NOS_CAP_CDT
    struct nos_cap_table_snapshot *snap;
    struct nos_cap_snapshot_entry *entries;
    uint32_t alloc_cap = 32U;
    uint16_t cap;
    uint16_t i;

    if (table == NULL) {
        return NULL;
    }

    snap = kzalloc(sizeof(*snap), GFP_KERNEL);
    if (snap == NULL) {
        return NULL;
    }

    spin_lock_irq(&table->lock);
    cap = cap_table_capacity_locked(table);
    alloc_cap = 0U;
    for (i = 1U; i < cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, i);
        if (slot != NULL && slot->type != NOS_CAP_NONE) {
            alloc_cap++;
        }
    }
    spin_unlock_irq(&table->lock);

    if (alloc_cap == 0U) {
        snap->owner_table = table;
        snap->entry_count = 0U;
        snap->entries = NULL;
        return snap;
    }

    /* 预留冗余空间防止锁释放期间少量新增句柄导致重分配 */
    alloc_cap += 16U;
    entries = kzalloc((uint32_t)(sizeof(*entries) * alloc_cap), GFP_KERNEL);
    if (entries == NULL) {
        kfree(snap);
        return NULL;
    }

    spin_lock_irq(&table->lock);
    cap = cap_table_capacity_locked(table);
    for (i = 1U; i < cap && snap->entry_count < alloc_cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, i);
        struct nos_cap_snapshot_entry *e;

        if (slot == NULL || slot->type == NOS_CAP_NONE) {
            continue;
        }
        if (slot->obj != NULL && cap_type_has_object_header(slot->type)) {
            struct nos_object *obj = (struct nos_object *)slot->obj;
            if (obj->retired || !nos_object_get(obj)) {
                continue;
            }
        }
        e = &entries[snap->entry_count++];
        e->slot_idx = i;
        e->type = slot->type;
        e->gen = slot->gen;
        e->rights = slot->rights;
        e->badge = slot->badge;
        e->obj = slot->obj;
        e->schema_mask = cap_schema_get_locked(table, i);
    }
    spin_unlock_irq(&table->lock);

    snap->owner_table = table;
    snap->entries = entries;
    return snap;
#else
    (void)table;
    return NULL;
#endif
}

int nos_cap_table_restore_ex(struct nos_cap_table *table,
                             const struct nos_cap_table_snapshot *snap,
                             const void *preserve_snap_obj)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint32_t i;
    uint16_t slot_idx;
    uint16_t cap;

    if (table == NULL || snap == NULL || snap->owner_table != table) {
        return -EINVAL;
    }

    /* 在持有全局 CDT 锁之前为快照中包含非零 schema_mask 的槽位预先补齐扩展存储页 */
    for (i = 0U; i < snap->entry_count; i++) {
        if (snap->entries[i].schema_mask != 0ULL) {
            int rc = cap_ensure_schema_slot(table, snap->entries[i].slot_idx);
            if (rc != 0) {
                return rc;
            }
        }
    }

    spin_lock_irq(&g_cdt_lock);
    spin_lock_irq(&table->lock);
    cap = cap_table_capacity_locked(table);

    for (slot_idx = 1U; slot_idx < cap; slot_idx++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(table, slot_idx);
        const struct nos_cap_snapshot_entry *match = NULL;

        if (slot == NULL) {
            continue;
        }
        for (i = 0U; i < snap->entry_count; i++) {
            if (snap->entries[i].slot_idx == slot_idx) {
                match = &snap->entries[i];
                break;
            }
        }

        if (match == NULL) {
            /* 若是当前正在用于回滚的 Cell 自检查点快照句柄本身，保留以支持多次投机重试 */
            if (preserve_snap_obj != NULL &&
                slot->type == NOS_CAP_SNAPSHOT &&
                slot->obj == preserve_snap_obj) {
                continue;
            }
            /* 分支期新分配或派生的句柄：摘除 CDT 并升代失效 */
            if (slot->type != NOS_CAP_NONE) {
                cap_cdt_detach_reparent_locked(slot);
                cap_clear_slot_preserve_owner(slot);
            }
            continue;
        }

        if (slot->type == match->type && slot->gen == match->gen && slot->obj == match->obj) {
            /* 未被销毁的同一句柄原位保留 CDT 关系，仅恢复权位、徽章与 schema_mask */
            slot->rights = match->rights;
            slot->badge = match->badge;
            {
                uint64_t *mp = cap_schema_ptr_locked(table, slot_idx);
                if (mp != NULL) {
                    *mp = match->schema_mask;
                }
            }
        } else {
            if (slot->type != NOS_CAP_NONE) {
                cap_cdt_detach_reparent_locked(slot);
                cap_clear_slot_preserve_owner(slot);
            }
            if (match->obj != NULL && cap_type_has_object_header(match->type) &&
                ((const struct nos_object *)match->obj)->retired) {
                continue;
            }
            slot->type = match->type;
            slot->gen = (match->gen == 0U) ? 1U : match->gen;
            slot->rights = match->rights;
            slot->badge = match->badge;
            slot->obj = match->obj;
            {
                uint64_t *mp = cap_schema_ptr_locked(table, slot_idx);
                if (mp != NULL) {
                    *mp = match->schema_mask;
                }
            }
        }
    }

    spin_unlock_irq(&table->lock);
    spin_unlock_irq(&g_cdt_lock);
    return 0;
#else
    (void)table;
    (void)snap;
    (void)preserve_snap_obj;
    return -ENOSYS;
#endif
}

int nos_cap_table_restore(struct nos_cap_table *table,
                          const struct nos_cap_table_snapshot *snap)
{
    return nos_cap_table_restore_ex(table, snap, NULL);
}

void nos_cap_table_snapshot_free(struct nos_cap_table_snapshot *snap)
{
#ifdef CONFIG_NOS_CAP_CDT
    uint32_t i;

    if (snap == NULL) {
        return;
    }
    for (i = 0U; i < snap->entry_count; i++) {
        struct nos_cap_snapshot_entry *e = &snap->entries[i];
        if (e->obj != NULL && cap_type_has_object_header(e->type)) {
            (void)nos_object_put((struct nos_object *)e->obj);
        }
    }
    if (snap->entries != NULL) {
        kfree(snap->entries);
    }
    kfree(snap);
#else
    (void)snap;
#endif
}

int nos_cap_table_clone_for_fork(struct nos_cap_table *src_table,
                                 struct nos_cap_table *dst_table)
{
#ifdef CONFIG_NOS_CAP_CDT
    nos_handle_t *handles;
    uint32_t max_h = 0U;
    uint32_t count = 0U;
    uint16_t cap;
    uint16_t i;

    if (src_table == NULL || dst_table == NULL || src_table == dst_table) {
        return -EINVAL;
    }

    spin_lock_irq(&src_table->lock);
    cap = cap_table_capacity_locked(src_table);
    for (i = 1U; i < cap; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(src_table, i);
        if (slot != NULL && slot->type != NOS_CAP_NONE &&
            (slot->rights & NOS_CAP_RIGHT_GRANT) != 0U &&
            slot->type != NOS_CAP_CELL && slot->type != NOS_CAP_SNAPSHOT) {
            max_h++;
        }
    }
    spin_unlock_irq(&src_table->lock);

    if (max_h == 0U) {
        return 0;
    }

    handles = kmalloc((uint32_t)(sizeof(nos_handle_t) * max_h), GFP_KERNEL);
    if (handles == NULL) {
        return -ENOMEM;
    }

    spin_lock_irq(&src_table->lock);
    cap = cap_table_capacity_locked(src_table);
    for (i = 1U; i < cap && count < max_h; i++) {
        struct nos_cap_slot *slot = cap_slot_ptr_locked(src_table, i);
        if (slot != NULL && slot->type != NOS_CAP_NONE &&
            (slot->rights & NOS_CAP_RIGHT_GRANT) != 0U &&
            slot->type != NOS_CAP_CELL && slot->type != NOS_CAP_SNAPSHOT) {
            handles[count++] = nos_handle_make(i, slot->gen);
        }
    }
    spin_unlock_irq(&src_table->lock);

    for (uint32_t j = 0U; j < count; j++) {
        int rc = nos_cap_derive(src_table, handles[j], dst_table, 0U, 0U, NULL);
        if (rc < 0 && rc != -EBADF && rc != -EACCES) {
            kfree(handles);
            return rc;
        }
    }

    kfree(handles);
    return 0;
#else
    (void)src_table;
    (void)dst_table;
    return -ENOSYS;
#endif
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
    if (idx == 0U || gen == 0U) {
        return -EBADF;
    }

    spin_lock_irq(&table->lock);
    slot = cap_slot_ptr_locked(table, idx);
    if (slot == NULL || slot->type == NOS_CAP_NONE || slot->gen != gen) {
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
