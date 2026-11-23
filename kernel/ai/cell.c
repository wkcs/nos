/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS Agent Cell 微容器安全边界实现：
 * - 独立 CNode 能力根与 TTBR0/ASID 地址空间
 * - 五维硬预算原子计量与单调收紧语义
 * - 4KB 只读用户态 Telemetry 共享页（零 Syscall 实时读取配额）
 * - 64B 定长不可伪造环形审计区（同步记录权限派生、撤销、异常与预算熔断）
 */

#define pr_fmt(fmt) "[CELL]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/cell.h>
#include <nos/vmo.h>
#include <nos/iommu.h>
#include <nos/notification.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <asm-generic/mmu.h>
#include <string.h>

#ifdef __HOST_TEST__
#include <stdlib.h>
#endif

static uint32_t g_next_cell_id = 1U;
static uint32_t g_cell_subsys_inited = 0U;

#define CELL_SID_MAP_CAP 16U
struct cell_sid_slot {
    bool             in_use;
    uint32_t         sid;
    struct nos_cell *cell;
};
static struct cell_sid_slot g_cell_sid_map[CELL_SID_MAP_CAP];
static spinlock_t g_cell_sid_lock;
static bool g_cell_sid_lock_inited;

__attribute__((weak)) void nos_iommu_register_fault_cb(nos_iommu_fault_cb_t cb)
{
    (void)cb;
}

static void cell_iommu_fault_cb(const struct nos_iommu_fault_event *evt)
{
    struct nos_cell *hit = NULL;
    uint32_t i;

    if (evt == NULL || !g_cell_sid_lock_inited) {
        return;
    }

    spin_lock_irq(&g_cell_sid_lock);
    for (i = 0U; i < CELL_SID_MAP_CAP; i++) {
        if (g_cell_sid_map[i].in_use && g_cell_sid_map[i].sid == evt->sid &&
            g_cell_sid_map[i].cell != NULL &&
            nos_object_get(&g_cell_sid_map[i].cell->header)) {
            hit = g_cell_sid_map[i].cell;
            break;
        }
    }
    spin_unlock_irq(&g_cell_sid_lock);

    if (hit != NULL) {
        nos_cell_record_audit(hit,
                              NOS_AUDIT_IOMMU_FAULT,
                              evt->iova,
                              evt->sid,
                              -EFAULT,
                              (uint64_t)evt->event_code,
                              (uint64_t)evt->pasid);
        nos_cell_notify_event(hit, NOS_CELL_EVT_IOMMU_FAULT);
        (void)nos_object_put(&hit->header);
    }
}

static void cell_unbind_iommu_sid_all(const struct nos_cell *cell)
{
    if (cell == NULL || !g_cell_sid_lock_inited) {
        return;
    }
    spin_lock_irq(&g_cell_sid_lock);
    for (uint32_t i = 0U; i < CELL_SID_MAP_CAP; i++) {
        if (g_cell_sid_map[i].in_use && g_cell_sid_map[i].cell == cell) {
            g_cell_sid_map[i].in_use = false;
            g_cell_sid_map[i].cell = NULL;
        }
    }
    spin_unlock_irq(&g_cell_sid_lock);
}

int nos_cell_bind_iommu_sid(struct nos_cell *cell, uint32_t sid)
{
    uint32_t i;
    int empty = -1;

    if (cell == NULL) {
        return -EINVAL;
    }
    if (!g_cell_sid_lock_inited) {
        spin_lock_init(&g_cell_sid_lock);
        g_cell_sid_lock_inited = true;
    }
    nos_iommu_register_fault_cb(cell_iommu_fault_cb);

    spin_lock_irq(&cell->lock);
    cell->iommu_sid = sid;
    cell->has_iommu_sid = true;
    spin_unlock_irq(&cell->lock);

    spin_lock_irq(&g_cell_sid_lock);
    for (i = 0U; i < CELL_SID_MAP_CAP; i++) {
        if (g_cell_sid_map[i].in_use && g_cell_sid_map[i].cell == cell &&
            g_cell_sid_map[i].sid == sid) {
            spin_unlock_irq(&g_cell_sid_lock);
            return 0;
        }
        if (!g_cell_sid_map[i].in_use && empty < 0) {
            empty = (int)i;
        }
    }
    if (empty >= 0) {
        g_cell_sid_map[empty].in_use = true;
        g_cell_sid_map[empty].sid = sid;
        g_cell_sid_map[empty].cell = cell;
    }
    spin_unlock_irq(&g_cell_sid_lock);
    return (empty >= 0) ? 0 : -ENOSPC;
}

static int cell_alloc_telemetry_page(uintptr_t *out_kv, uintptr_t *out_pa)
{
#ifdef __HOST_TEST__
    void *mem = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (mem == NULL) {
        return -ENOMEM;
    }
    memset(mem, 0, PAGE_SIZE);
    *out_kv = (uintptr_t)mem;
    *out_pa = (uintptr_t)mem;
    return 0;
#else
    addr_t va = alloc_page(GFP_KERNEL | GFP_ZERO);
    uintptr_t pa;

    if (va == 0U) {
        return -ENOMEM;
    }
    pa = (uintptr_t)arch_virt_to_phys((unsigned long)va);
    if (pa == 0U) {
        (void)free_page(va);
        return -EFAULT;
    }
    *out_kv = (uintptr_t)va;
    *out_pa = pa;
    return 0;
#endif
}

static void cell_free_telemetry_page(uintptr_t kv)
{
    if (kv == 0U) {
        return;
    }
#ifdef __HOST_TEST__
    free((void *)kv);
#else
    (void)free_page((addr_t)kv);
#endif
}

/* 在 cell->lock 保护下将当前配额与使用量实时投影到 4KB 只读 Telemetry 页 */
static void cell_sync_telemetry_locked(struct nos_cell *cell)
{
    struct nos_cell_telemetry *tm;

    if (cell == NULL || cell->telemetry_kvaddr == 0U) {
        return;
    }

    tm = (struct nos_cell_telemetry *)cell->telemetry_kvaddr;
    tm->magic = NOS_CELL_TELEMETRY_MAGIC;
    tm->version = 1U;
    tm->state = cell->state;
    tm->cell_id = cell->cell_id;
    tm->child_cells_used = cell->child_cells_used;
    tm->cpu_ns_used = cell->cpu_ns_used;
    tm->mem_bytes_used = cell->mem_bytes_used;
    tm->tool_calls_used = cell->tool_calls_used;
    tm->accel_ops_used = cell->accel_ops_used;
    tm->audit_seq = cell->audit_seq;
    tm->limit = cell->budget;
    smp_wmb();
}

static void cell_append_audit_locked(struct nos_cell *cell,
                                     uint16_t event_type,
                                     uint64_t schema_id,
                                     uint32_t badge,
                                     int32_t status,
                                     uint64_t arg0,
                                     uint64_t arg1)
{
    struct nos_audit_record *rec = &cell->audit_ring[cell->audit_head];

    rec->timestamp_ns = hal_timer_monotonic_ns();
    rec->schema_id = schema_id;
    rec->cell_id = cell->cell_id;
    rec->event_type = event_type;
    rec->flags = (status < 0) ? 1U : 0U;
    rec->status = status;
    rec->cap_badge = badge;
    rec->arg0 = arg0;
    rec->arg1 = arg1;
    rec->reserved[0] = 0ULL;
    rec->reserved[1] = 0ULL;

    cell->audit_head = (cell->audit_head + 1U) % NOS_CELL_AUDIT_RING_CAPACITY;
    if (cell->audit_count < NOS_CELL_AUDIT_RING_CAPACITY) {
        cell->audit_count++;
    }
    cell->audit_seq++;
    cell_sync_telemetry_locked(cell);
}

static uint64_t cell_clamp_u64_limit(uint64_t parent_limit, uint64_t req_limit)
{
    if (parent_limit == 0ULL) {
        return req_limit;
    }
    if (req_limit == 0ULL || req_limit > parent_limit) {
        return parent_limit;
    }
    return req_limit;
}

static void cell_apply_monotonic_budget(struct nos_cell_budget *dst,
                                        const struct nos_cell_budget *parent,
                                        const struct nos_cell_budget *req)
{
    struct nos_cell_budget eff;

    if (req != NULL) {
        eff = *req;
    } else {
        memset(&eff, 0, sizeof(eff));
        if (parent == NULL) {
            eff.max_child_cells = UINT32_MAX;
        } else {
            eff = *parent;
        }
    }

    if (parent != NULL) {
        eff.cpu_ns_limit = cell_clamp_u64_limit(parent->cpu_ns_limit, eff.cpu_ns_limit);
        eff.mem_bytes_limit = cell_clamp_u64_limit(parent->mem_bytes_limit, eff.mem_bytes_limit);
        eff.tool_calls_limit = cell_clamp_u64_limit(parent->tool_calls_limit, eff.tool_calls_limit);
        eff.accel_ops_limit = cell_clamp_u64_limit(parent->accel_ops_limit, eff.accel_ops_limit);
        if (eff.max_child_cells > parent->max_child_cells) {
            eff.max_child_cells = parent->max_child_cells;
        }
    }

    *dst = eff;
}

static void cell_cap_audit_hook(void *cell_ptr,
                                uint16_t event_type,
                                uint64_t obj_id,
                                uint32_t badge,
                                int32_t status,
                                uint64_t arg0,
                                uint64_t arg1)
{
    struct nos_cell *cell = (struct nos_cell *)cell_ptr;

    if (cell == NULL) {
        return;
    }
    nos_cell_record_audit(cell, event_type, obj_id, badge, status, arg0, arg1);
}

static void cell_release(struct nos_object *obj)
{
    struct nos_cell *cell = (struct nos_cell *)obj;
    struct nos_cell *parent;
    struct nos_notification *notif;

    if (cell == NULL) {
        return;
    }

    parent = cell->parent;
    notif = cell->budget_notif;
    cell->parent = NULL;
    cell->budget_notif = NULL;
    cell_unbind_iommu_sid_all(cell);

    for (uint32_t i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        if (cell->vmo_bindings[i].active && cell->vmo_bindings[i].vmo != NULL) {
            (void)nos_object_put(&cell->vmo_bindings[i].vmo->header);
            cell->vmo_bindings[i].active = false;
            cell->vmo_bindings[i].vmo = NULL;
        }
    }

    if (cell->telemetry_kvaddr != 0U) {
        cell_free_telemetry_page(cell->telemetry_kvaddr);
        cell->telemetry_kvaddr = 0U;
        cell->telemetry_paddr = 0U;
    }

    hal_vm_space_destroy(&cell->vm_space);
    nos_cap_table_destroy(&cell->cap_table);

    if (notif != NULL) {
        (void)nos_object_put(&notif->object);
    }
    if (parent != NULL) {
        if (!cell->parent_accounted) {
            spin_lock_irq(&parent->lock);
            if (parent->child_cells_used > 0U) {
                parent->child_cells_used--;
            }
            cell_sync_telemetry_locked(parent);
            spin_unlock_irq(&parent->lock);
        }
        (void)nos_object_put(&parent->header);
    }

    kfree(cell);
}

static struct nos_notification *cell_trigger_budget_hit_locked(struct nos_cell *cell,
                                                               uint64_t schema_id,
                                                               uint32_t badge,
                                                               uint64_t attempted,
                                                               uint64_t limit)
{
    struct nos_notification *notif = NULL;

    cell->state = NOS_CELL_STATE_BUDGET_HIT;
    cell_append_audit_locked(cell,
                             NOS_AUDIT_BUDGET_HIT,
                             schema_id,
                             badge,
                             -EDQUOT,
                             attempted,
                             limit);
    if (cell->budget_notif != NULL && nos_object_get(&cell->budget_notif->object)) {
        notif = cell->budget_notif;
    }
    return notif;
}

static bool cell_is_stopped_state(uint16_t state)
{
    return (state == NOS_CELL_STATE_DESTROYED || state == NOS_CELL_STATE_SUSPENDED);
}

static void cell_notify_budget_hit(struct nos_notification *notif, uint64_t evt_bits)
{
    if (notif != NULL) {
        (void)nos_notify_signal_obj(notif, (evt_bits != 0ULL) ? evt_bits : 1ULL);
        (void)nos_object_put(&notif->object);
    }
}

void nos_cell_notify_event(struct nos_cell *cell, uint64_t evt_bits)
{
    struct nos_notification *notif = NULL;

    if (cell == NULL || evt_bits == 0ULL) {
        return;
    }

    spin_lock_irq(&cell->lock);
    if (cell->budget_notif != NULL && nos_object_get(&cell->budget_notif->object)) {
        notif = cell->budget_notif;
    }
    spin_unlock_irq(&cell->lock);

    if (notif != NULL) {
        (void)nos_notify_signal_obj(notif, evt_bits);
        (void)nos_object_put(&notif->object);
    }
}

void nos_cell_subsystem_init(void)
{
    if (__atomic_exchange_n(&g_cell_subsys_inited, 1U, __ATOMIC_ACQ_REL) == 0U) {
        if (!g_cell_sid_lock_inited) {
            spin_lock_init(&g_cell_sid_lock);
            g_cell_sid_lock_inited = true;
        }
        nos_cap_register_audit_hook(cell_cap_audit_hook);
        nos_iommu_register_fault_cb(cell_iommu_fault_cb);
    }
}

struct nos_cell *nos_cell_create(struct nos_cell *parent,
                                 const struct nos_cell_budget *budget)
{
    struct nos_cell *cell;
    struct nos_cell_budget parent_budget;
    uintptr_t tm_kv = 0U;
    uintptr_t tm_pa = 0U;

    nos_cell_subsystem_init();
    memset(&parent_budget, 0, sizeof(parent_budget));

    if (parent != NULL) {
        struct nos_notification *notif = NULL;

        if (!nos_object_get(&parent->header)) {
            return NULL;
        }
        spin_lock_irq(&parent->lock);
        if (cell_is_stopped_state(parent->state)) {
            spin_unlock_irq(&parent->lock);
            (void)nos_object_put(&parent->header);
            return NULL;
        }
        if (parent->child_cells_used >= parent->budget.max_child_cells) {
            notif = cell_trigger_budget_hit_locked(parent,
                                                   0ULL,
                                                   0U,
                                                   (uint64_t)parent->child_cells_used + 1ULL,
                                                   (uint64_t)parent->budget.max_child_cells);
            spin_unlock_irq(&parent->lock);
            cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_CPU);
            (void)nos_object_put(&parent->header);
            return NULL;
        }
        parent->child_cells_used++;
        parent_budget = parent->budget;
        cell_sync_telemetry_locked(parent);
        spin_unlock_irq(&parent->lock);
    }

    cell = kzalloc(sizeof(*cell), GFP_KERNEL);
    if (cell == NULL) {
        goto err_uncount_parent;
    }

    if (cell_alloc_telemetry_page(&tm_kv, &tm_pa) != 0) {
        kfree(cell);
        goto err_uncount_parent;
    }

    if (hal_vm_space_init(&cell->vm_space) != 0) {
        cell_free_telemetry_page(tm_kv);
        kfree(cell);
        goto err_uncount_parent;
    }

    (void)nos_object_init(&cell->header, false);
    nos_object_set_release(&cell->header, cell_release);
    spin_lock_init(&cell->lock);
    nos_cap_table_init(&cell->cap_table);
    nos_cap_table_bind_cell(&cell->cap_table, cell);

    cell->cell_id = __atomic_fetch_add(&g_next_cell_id, 1U, __ATOMIC_RELAXED);
    cell->state = NOS_CELL_STATE_ACTIVE;
    cell->parent = parent;
    cell->telemetry_kvaddr = tm_kv;
    cell->telemetry_paddr = tm_pa;
    cell_apply_monotonic_budget(&cell->budget, (parent != NULL) ? &parent_budget : NULL, budget);
    cell_sync_telemetry_locked(cell);
    return cell;

err_uncount_parent:
    if (parent != NULL) {
        spin_lock_irq(&parent->lock);
        if (parent->child_cells_used > 0U) {
            parent->child_cells_used--;
        }
        cell_sync_telemetry_locked(parent);
        spin_unlock_irq(&parent->lock);
        (void)nos_object_put(&parent->header);
    }
    return NULL;
}

struct nos_cell *nos_cell_fork(struct nos_cell *parent,
                               const struct nos_cell_budget *budget)
{
    struct nos_cell_budget rem_budget;
    struct nos_cell_budget eff_budget;
    struct nos_cell *child;
    struct nos_cell_vmo_binding parent_vmos[NOS_CELL_MAX_TRACKED_VMOS];
    uint32_t vmo_cnt = 0U;
    uint32_t i;

    if (parent == NULL) {
        return NULL;
    }

    memset(&rem_budget, 0, sizeof(rem_budget));
    memset(parent_vmos, 0, sizeof(parent_vmos));

    spin_lock_irq(&parent->lock);
    if (cell_is_stopped_state(parent->state) ||
        parent->child_cells_used >= parent->budget.max_child_cells) {
        spin_unlock_irq(&parent->lock);
        return NULL;
    }

    /* 若父 Cell 任意受限硬预算已耗尽，禁止 Fork 绕过配额并触发审计 */
    if ((parent->budget.cpu_ns_limit != 0ULL &&
         parent->cpu_ns_used >= parent->budget.cpu_ns_limit) ||
        (parent->budget.mem_bytes_limit != 0ULL &&
         parent->mem_bytes_used >= parent->budget.mem_bytes_limit) ||
        (parent->budget.tool_calls_limit != 0ULL &&
         parent->tool_calls_used >= parent->budget.tool_calls_limit) ||
        (parent->budget.accel_ops_limit != 0ULL &&
         parent->accel_ops_used >= parent->budget.accel_ops_limit)) {
        struct nos_notification *notif = cell_trigger_budget_hit_locked(
            parent, 0ULL, 0U, parent->mem_bytes_used, parent->budget.mem_bytes_limit);
        spin_unlock_irq(&parent->lock);
        cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_MEM);
        return NULL;
    }

    /* 以父级剩余可用配额作为单调上界夹紧子 Cell 预算 */
    rem_budget.cpu_ns_limit = (parent->budget.cpu_ns_limit != 0ULL)
                                  ? (parent->budget.cpu_ns_limit - parent->cpu_ns_used)
                                  : 0ULL;
    rem_budget.mem_bytes_limit = (parent->budget.mem_bytes_limit != 0ULL)
                                     ? (parent->budget.mem_bytes_limit - parent->mem_bytes_used)
                                     : 0ULL;
    rem_budget.tool_calls_limit = (parent->budget.tool_calls_limit != 0ULL)
                                      ? (parent->budget.tool_calls_limit - parent->tool_calls_used)
                                      : 0ULL;
    rem_budget.accel_ops_limit = (parent->budget.accel_ops_limit != 0ULL)
                                     ? (parent->budget.accel_ops_limit - parent->accel_ops_used)
                                     : 0ULL;
    rem_budget.max_child_cells = parent->budget.max_child_cells - parent->child_cells_used - 1U;

    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        if (parent->vmo_bindings[i].active && parent->vmo_bindings[i].vmo != NULL &&
            nos_object_get(&parent->vmo_bindings[i].vmo->header)) {
            parent_vmos[vmo_cnt++] = parent->vmo_bindings[i];
        }
    }
    spin_unlock_irq(&parent->lock);

    cell_apply_monotonic_budget(&eff_budget, &rem_budget, budget);
    child = nos_cell_create(parent, &eff_budget);
    if (child == NULL) {
        for (i = 0U; i < vmo_cnt; i++) {
            (void)nos_object_put(&parent_vmos[i].vmo->header);
        }
        return NULL;
    }

    /* 1. 复制地址空间绑定：只读权重直接共享同一 VMO；可写页启用 CoW 克隆 */
    for (i = 0U; i < vmo_cnt; i++) {
        struct nos_cell_vmo_binding *b = &parent_vmos[i];
        bool writable = (b->prot_flags & HAL_VM_PROT_WRITE) != 0U &&
                        !b->vmo->sealed &&
                        b->vmo->kind != NOS_VMO_WEIGHT;
        int rc = 0;

        if (writable && (b->vmo->kind == NOS_VMO_ANON || b->vmo->kind == NOS_VMO_KVCACHE)) {
            struct nos_vmo *cow_vmo = nos_vmo_clone_cow(b->vmo);
            if (cow_vmo == NULL) {
                rc = -ENOMEM;
            } else {
                /* 先将父 Cell 页表降权 RO 武装 CoW，防止父后续写脏子页 */
                rc = nos_vmo_remap_range(b->vmo,
                                         &parent->vm_space,
                                         b->vaddr,
                                         b->offset,
                                         b->len,
                                         b->prot_flags & ~HAL_VM_PROT_WRITE);
                if (rc == 0) {
                    rc = nos_vmo_map_tracked_nocharge(cow_vmo,
                                                      child,
                                                      b->vaddr,
                                                      b->offset,
                                                      b->len,
                                                      b->prot_flags & ~HAL_VM_PROT_WRITE,
                                                      b->prot_flags);
                }
                /* 交出初始创建者引用：仅由 child->vmo_bindings 持有并保持可被 CoW/Snapshot get */
                (void)nos_object_put(&cow_vmo->header);
            }
        } else if ((b->vmo->flags & NOS_VMO_FLAG_IO_COHERENT) == 0U &&
                   b->vmo->kind != NOS_VMO_DMA_TENSOR) {
            uint32_t ro_prot = b->prot_flags & ~HAL_VM_PROT_WRITE;
            rc = nos_vmo_map_tracked_nocharge(b->vmo,
                                              child,
                                              b->vaddr,
                                              b->offset,
                                              b->len,
                                              ro_prot,
                                              ro_prot);
        }

        if (rc != 0) {
            for (uint32_t j = i; j < vmo_cnt; j++) {
                (void)nos_object_put(&parent_vmos[j].vmo->header);
            }
            (void)nos_cell_destroy(child);
            return NULL;
        }
        (void)nos_object_put(&b->vmo->header);
    }

    /* 2. 派生父 CNode 中允许传播的能力句柄，并在父 CDT 建立父子依赖以便父销毁时连根吊销 */
    if (nos_cap_table_clone_for_fork(&parent->cap_table, &child->cap_table) != 0) {
        (void)nos_cell_destroy(child);
        return NULL;
    }

    return child;
}

int nos_cell_destroy(struct nos_cell *cell)
{
    struct nos_cell *parent = NULL;
    bool uncount_parent = false;
    int rc;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    cell->state = NOS_CELL_STATE_DESTROYED;
    if (cell->parent != NULL && !cell->parent_accounted) {
        parent = cell->parent;
        cell->parent_accounted = true;
        uncount_parent = true;
    }
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);

    if (uncount_parent && parent != NULL) {
        spin_lock_irq(&parent->lock);
        if (parent->child_cells_used > 0U) {
            parent->child_cells_used--;
        }
        cell_sync_telemetry_locked(parent);
        spin_unlock_irq(&parent->lock);
    }

    (void)nos_object_close(&cell->header);
    rc = nos_object_retire(&cell->header);
    return (rc == -EINVAL) ? 0 : rc;
}

int nos_cell_set_budget(struct nos_cell *cell, const struct nos_cell_budget *budget)
{
    struct nos_cell_budget parent_budget;
    bool has_parent = false;

    if (cell == NULL || budget == NULL) {
        return -EINVAL;
    }

    if (cell->parent != NULL) {
        spin_lock_irq(&cell->parent->lock);
        parent_budget = cell->parent->budget;
        has_parent = true;
        spin_unlock_irq(&cell->parent->lock);
    }

    spin_lock_irq(&cell->lock);
    if (cell->state == NOS_CELL_STATE_DESTROYED) {
        spin_unlock_irq(&cell->lock);
        return -ESTALE;
    }
    cell_apply_monotonic_budget(&cell->budget, has_parent ? &parent_budget : NULL, budget);
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);
    return 0;
}

int nos_cell_get_budget(struct nos_cell *cell, struct nos_cell_budget *out_budget)
{
    if (cell == NULL || out_budget == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    *out_budget = cell->budget;
    spin_unlock_irq(&cell->lock);
    return 0;
}

int nos_cell_bind_budget_notif(struct nos_cell *cell, struct nos_notification *notif)
{
    struct nos_notification *old_notif;

    if (cell == NULL) {
        return -EINVAL;
    }
    if (notif != NULL && !nos_object_get(&notif->object)) {
        return -ESTALE;
    }

    spin_lock_irq(&cell->lock);
    old_notif = cell->budget_notif;
    cell->budget_notif = notif;
    spin_unlock_irq(&cell->lock);

    if (old_notif != NULL) {
        (void)nos_object_put(&old_notif->object);
    }
    return 0;
}

int nos_cell_map_telemetry(struct nos_cell *cell, uintptr_t user_vaddr)
{
    uintptr_t paddr;

    if (cell == NULL || user_vaddr == 0U || (user_vaddr & (PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    paddr = cell->telemetry_paddr;
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);

    if (paddr == 0U) {
        return -EINVAL;
    }

    /* Telemetry 共享页向 EL0 严格映射为只读不可写、不可执行 (R--) */
    return hal_vm_map(&cell->vm_space,
                      user_vaddr,
                      paddr,
                      PAGE_SIZE,
                      HAL_VM_PROT_USER | HAL_VM_PROT_READ);
}

const struct nos_cell_telemetry *nos_cell_telemetry_view(const struct nos_cell *cell)
{
    if (cell == NULL) {
        return NULL;
    }
    return (const struct nos_cell_telemetry *)cell->telemetry_kvaddr;
}

int nos_cell_charge_cpu(struct nos_cell *cell, uint64_t delta_ns)
{
    struct nos_notification *notif = NULL;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    if (cell_is_stopped_state(cell->state)) {
        spin_unlock_irq(&cell->lock);
        return -EDQUOT;
    }
    if (cell->budget.cpu_ns_limit != 0ULL &&
        (delta_ns > cell->budget.cpu_ns_limit ||
         cell->cpu_ns_used > cell->budget.cpu_ns_limit - delta_ns)) {
        uint64_t attempted = cell->cpu_ns_used + delta_ns;
        notif = cell_trigger_budget_hit_locked(cell,
                                               0ULL,
                                               0U,
                                               attempted,
                                               cell->budget.cpu_ns_limit);
        spin_unlock_irq(&cell->lock);
        cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_CPU);
        return -EDQUOT;
    }
    cell->cpu_ns_used += delta_ns;
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);
    return 0;
}

int nos_cell_charge_mem(struct nos_cell *cell, uint64_t bytes)
{
    struct nos_notification *notif = NULL;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    if (cell_is_stopped_state(cell->state)) {
        spin_unlock_irq(&cell->lock);
        return -EDQUOT;
    }
    if (cell->budget.mem_bytes_limit != 0ULL &&
        (bytes > cell->budget.mem_bytes_limit ||
         cell->mem_bytes_used > cell->budget.mem_bytes_limit - bytes)) {
        uint64_t attempted = cell->mem_bytes_used + bytes;
        notif = cell_trigger_budget_hit_locked(cell,
                                               0ULL,
                                               0U,
                                               attempted,
                                               cell->budget.mem_bytes_limit);
        spin_unlock_irq(&cell->lock);
        cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_MEM);
        return -EDQUOT;
    }
    cell->mem_bytes_used += bytes;
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);
    return 0;
}

void nos_cell_uncharge_mem(struct nos_cell *cell, uint64_t bytes)
{
    if (cell == NULL || bytes == 0ULL) {
        return;
    }

    spin_lock_irq(&cell->lock);
    if (cell->mem_bytes_used >= bytes) {
        cell->mem_bytes_used -= bytes;
    } else {
        cell->mem_bytes_used = 0ULL;
    }
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);
}

int nos_cell_charge_tool_call(struct nos_cell *cell, uint64_t schema_id, uint32_t badge)
{
    struct nos_notification *notif = NULL;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    if (cell_is_stopped_state(cell->state)) {
        spin_unlock_irq(&cell->lock);
        return -EDQUOT;
    }
    if (cell->budget.tool_calls_limit != 0ULL &&
        cell->tool_calls_used >= cell->budget.tool_calls_limit) {
        notif = cell_trigger_budget_hit_locked(cell,
                                               schema_id,
                                               badge,
                                               cell->tool_calls_used + 1ULL,
                                               cell->budget.tool_calls_limit);
        spin_unlock_irq(&cell->lock);
        cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_TOOL);
        return -EDQUOT;
    }
    cell->tool_calls_used++;
    cell_append_audit_locked(cell,
                             NOS_AUDIT_TOOL_CALL,
                             schema_id,
                             badge,
                             0,
                             cell->tool_calls_used,
                             cell->budget.tool_calls_limit);
    spin_unlock_irq(&cell->lock);
    return 0;
}

int nos_cell_charge_accel_op(struct nos_cell *cell, uint64_t schema_id, uint32_t badge)
{
    struct nos_notification *notif = NULL;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    if (cell_is_stopped_state(cell->state)) {
        spin_unlock_irq(&cell->lock);
        return -EDQUOT;
    }
    if (cell->budget.accel_ops_limit != 0ULL &&
        cell->accel_ops_used >= cell->budget.accel_ops_limit) {
        notif = cell_trigger_budget_hit_locked(cell,
                                               schema_id,
                                               badge,
                                               cell->accel_ops_used + 1ULL,
                                               cell->budget.accel_ops_limit);
        spin_unlock_irq(&cell->lock);
        cell_notify_budget_hit(notif, NOS_CELL_EVT_BUDGET_ACCEL);
        return -EDQUOT;
    }
    cell->accel_ops_used++;
    cell_append_audit_locked(cell,
                             NOS_AUDIT_ACCEL_JOB,
                             schema_id,
                             badge,
                             0,
                             cell->accel_ops_used,
                             cell->budget.accel_ops_limit);
    spin_unlock_irq(&cell->lock);
    return 0;
}

void nos_cell_record_audit(struct nos_cell *cell,
                           uint16_t event_type,
                           uint64_t schema_id,
                           uint32_t badge,
                           int32_t status,
                           uint64_t arg0,
                           uint64_t arg1)
{
    if (cell == NULL) {
        return;
    }

    spin_lock_irq(&cell->lock);
    cell_append_audit_locked(cell, event_type, schema_id, badge, status, arg0, arg1);
    spin_unlock_irq(&cell->lock);
}

int nos_cell_read_audit(struct nos_cell *cell,
                        struct nos_audit_record *out_buf,
                        uint32_t max_records)
{
    uint32_t to_copy;
    uint32_t start;
    uint32_t i;

    if (cell == NULL || out_buf == NULL || max_records == 0U) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    to_copy = (cell->audit_count < max_records) ? cell->audit_count : max_records;
    if (cell->audit_count < NOS_CELL_AUDIT_RING_CAPACITY) {
        start = 0U;
    } else {
        start = cell->audit_head;
    }
    for (i = 0U; i < to_copy; i++) {
        uint32_t idx = (start + i) % NOS_CELL_AUDIT_RING_CAPACITY;
        out_buf[i] = cell->audit_ring[idx];
    }
    spin_unlock_irq(&cell->lock);

    return (int)to_copy;
}

struct nos_vm_space *nos_cell_vm_space(struct nos_cell *cell)
{
    return (cell != NULL) ? &cell->vm_space : NULL;
}

struct nos_cap_table *nos_cell_cap_table(struct nos_cell *cell)
{
    return (cell != NULL) ? &cell->cap_table : NULL;
}

int nos_cell_attach_task(struct task_struct *task, struct nos_cell *cell)
{
    struct nos_cell *old_cell;

    if (task == NULL) {
        return -EINVAL;
    }
    if (cell != NULL && !nos_object_get(&cell->header)) {
        return -ESTALE;
    }

    old_cell = task->cell;
    task->cell = cell;

    if (task == current) {
        hal_vm_switch((cell != NULL) ? &cell->vm_space : &task->vm_space);
    }
    if (old_cell != NULL) {
        (void)nos_object_put(&old_cell->header);
    }
    return 0;
}

void nos_cell_detach_task(struct task_struct *task)
{
    struct nos_cell *old_cell;

    if (task == NULL) {
        return;
    }

    old_cell = task->cell;
    task->cell = NULL;
    if (old_cell != NULL) {
        (void)nos_object_put(&old_cell->header);
    }
}

static bool cell_vmo_should_snapshot(const struct nos_cell_vmo_binding *b)
{
    if (!b->active || b->vmo == NULL) {
        return false;
    }
    if ((b->prot_flags & HAL_VM_PROT_WRITE) == 0U) {
        return false;
    }
    if ((b->vmo->flags & NOS_VMO_FLAG_IO_COHERENT) != 0U) {
        return false;
    }
    return (b->vmo->kind == NOS_VMO_ANON || b->vmo->kind == NOS_VMO_KVCACHE);
}

int nos_cell_track_vmo_map(struct nos_cell *cell,
                           struct nos_vmo *vmo,
                           uintptr_t vaddr,
                           size_t offset,
                           size_t len,
                           uint32_t prot_flags)
{
    uint32_t i;
    int empty_slot = -1;

    if (cell == NULL || vmo == NULL || len == 0U) {
        return -EINVAL;
    }
    if (!nos_object_get(&vmo->header)) {
        return -ESTALE;
    }

    spin_lock_irq(&cell->lock);
    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        if (cell->vmo_bindings[i].active) {
            if (cell->vmo_bindings[i].vaddr == vaddr && cell->vmo_bindings[i].len == len) {
                struct nos_vmo *old_vmo = cell->vmo_bindings[i].vmo;
                cell->vmo_bindings[i].vmo = vmo;
                cell->vmo_bindings[i].offset = offset;
                cell->vmo_bindings[i].prot_flags = prot_flags;
                spin_unlock_irq(&cell->lock);
                if (old_vmo != NULL) {
                    (void)nos_object_put(&old_vmo->header);
                }
                return 0;
            }
        } else if (empty_slot < 0) {
            empty_slot = (int)i;
        }
    }

    if (empty_slot < 0) {
        spin_unlock_irq(&cell->lock);
        (void)nos_object_put(&vmo->header);
        return -ENOSPC;
    }

    cell->vmo_bindings[empty_slot].active = true;
    cell->vmo_bindings[empty_slot].vmo = vmo;
    cell->vmo_bindings[empty_slot].vaddr = vaddr;
    cell->vmo_bindings[empty_slot].offset = offset;
    cell->vmo_bindings[empty_slot].len = len;
    cell->vmo_bindings[empty_slot].prot_flags = prot_flags;
    spin_unlock_irq(&cell->lock);
    return 0;
}

void nos_cell_untrack_vmo_map(struct nos_cell *cell, uintptr_t vaddr, size_t len)
{
    struct nos_vmo *released[NOS_CELL_MAX_TRACKED_VMOS];
    uint32_t rel_cnt = 0U;
    uint32_t i;

    if (cell == NULL || len == 0U) {
        return;
    }

    spin_lock_irq(&cell->lock);
    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        struct nos_cell_vmo_binding *b = &cell->vmo_bindings[i];
        if (b->active && b->vaddr >= vaddr && (b->vaddr + b->len) <= (vaddr + len)) {
            released[rel_cnt++] = b->vmo;
            b->active = false;
            b->vmo = NULL;
        }
    }
    spin_unlock_irq(&cell->lock);

    for (i = 0U; i < rel_cnt; i++) {
        if (released[i] != NULL) {
            (void)nos_object_put(&released[i]->header);
        }
    }
}

static void cell_snapshot_release(struct nos_object *obj)
{
    struct nos_cell_snapshot *snap = (struct nos_cell_snapshot *)obj;
    uint32_t i;

    if (snap == NULL) {
        return;
    }

    for (i = 0U; i < snap->item_count; i++) {
        if (snap->items[i].frozen_vmo != NULL) {
            (void)nos_object_close(&snap->items[i].frozen_vmo->header);
            (void)nos_object_retire(&snap->items[i].frozen_vmo->header);
            snap->items[i].frozen_vmo = NULL;
        }
        if (snap->items[i].live_vmo != NULL) {
            (void)nos_object_put(&snap->items[i].live_vmo->header);
            snap->items[i].live_vmo = NULL;
        }
    }

    if (snap->cap_snap != NULL) {
        nos_cap_table_snapshot_free(snap->cap_snap);
        snap->cap_snap = NULL;
    }

    if (snap->owner_cell != NULL) {
        (void)nos_object_put(&snap->owner_cell->header);
        snap->owner_cell = NULL;
    }

    kfree(snap);
}

struct nos_cell_snapshot *nos_cell_snapshot(struct nos_cell *cell)
{
    struct nos_cell_snapshot *snap;
    struct nos_cell_vmo_binding local[NOS_CELL_MAX_TRACKED_VMOS];
    uint64_t binding_mask = 0ULL;
    uint32_t count = 0U;
    uint32_t i;

    if (cell == NULL || !nos_object_get(&cell->header)) {
        return NULL;
    }

    snap = kzalloc(sizeof(*snap), GFP_KERNEL);
    if (snap == NULL) {
        (void)nos_object_put(&cell->header);
        return NULL;
    }

    memset(local, 0, sizeof(local));
    spin_lock_irq(&cell->lock);
    if (cell_is_stopped_state(cell->state)) {
        spin_unlock_irq(&cell->lock);
        kfree(snap);
        (void)nos_object_put(&cell->header);
        return NULL;
    }

    snap->saved_state = cell->state;
    snap->saved_cpu_ns_used = cell->cpu_ns_used;
    snap->saved_mem_bytes_used = cell->mem_bytes_used;
    snap->saved_tool_calls_used = cell->tool_calls_used;
    snap->saved_accel_ops_used = cell->accel_ops_used;

    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        if (cell->vmo_bindings[i].active) {
            binding_mask |= (1ULL << i);
        }
        if (cell_vmo_should_snapshot(&cell->vmo_bindings[i]) &&
            nos_object_get(&cell->vmo_bindings[i].vmo->header)) {
            local[count++] = cell->vmo_bindings[i];
        }
    }
    snap->saved_binding_mask = binding_mask;
    spin_unlock_irq(&cell->lock);

    (void)nos_object_init(&snap->header, false);
    nos_object_set_release(&snap->header, cell_snapshot_release);
    snap->owner_cell = cell;

#ifdef CONFIG_NOS_CAP_CDT
    snap->cap_snap = nos_cap_table_snapshot(&cell->cap_table);
    if (snap->cap_snap == NULL) {
        i = 0U;
        goto err_cleanup;
    }
#endif

    for (i = 0U; i < count; i++) {
        struct nos_vmo *frozen = nos_vmo_clone_cow(local[i].vmo);
        int rc;

        if (frozen == NULL || nos_vmo_seal(frozen) != 0) {
            if (frozen != NULL) {
                (void)nos_object_close(&frozen->header);
                (void)nos_object_retire(&frozen->header);
            }
            goto err_cleanup;
        }

        /* 将现网可写 PTE 降权为只读，下一次分支写入将触发硬件 CoW 页断裂 */
        rc = nos_vmo_remap_range(local[i].vmo,
                                 &cell->vm_space,
                                 local[i].vaddr,
                                 local[i].offset,
                                 local[i].len,
                                 local[i].prot_flags & ~HAL_VM_PROT_WRITE);
        if (rc != 0) {
            (void)nos_object_close(&frozen->header);
            (void)nos_object_retire(&frozen->header);
            goto err_cleanup;
        }

        snap->items[snap->item_count].live_vmo = local[i].vmo;
        snap->items[snap->item_count].frozen_vmo = frozen;
        snap->items[snap->item_count].vaddr = local[i].vaddr;
        snap->items[snap->item_count].offset = local[i].offset;
        snap->items[snap->item_count].len = local[i].len;
        snap->items[snap->item_count].prot_flags = local[i].prot_flags;
        snap->item_count++;
        local[i].vmo = NULL;
    }

    nos_cell_record_audit(cell,
                          NOS_AUDIT_CELL_SNAPSHOT,
                          (uint64_t)(uintptr_t)snap,
                          0U,
                          0,
                          (uint64_t)snap->item_count,
                          snap->saved_mem_bytes_used);
    return snap;

err_cleanup:
    for (; i < count; i++) {
        if (local[i].vmo != NULL) {
            (void)nos_object_put(&local[i].vmo->header);
        }
    }
    (void)nos_object_close(&snap->header);
    (void)nos_object_retire(&snap->header);
    return NULL;
}

int nos_cell_handle_cow_fault(struct nos_cell *cell, uintptr_t fault_va)
{
    struct nos_vmo *vmo = NULL;
    uintptr_t page_va = 0U;
    uint32_t page_idx = 0U;
    uint32_t orig_prot = 0U;
    uintptr_t new_pa = 0U;
    uint32_t i;
    int rc;

    if (cell == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        struct nos_cell_vmo_binding *b = &cell->vmo_bindings[i];

        if (!cell_vmo_should_snapshot(b)) {
            continue;
        }
        if (fault_va >= b->vaddr && fault_va < (b->vaddr + b->len)) {
            if (nos_object_get(&b->vmo->header)) {
                vmo = b->vmo;
                page_va = fault_va & ~(uintptr_t)(PAGE_SIZE - 1U);
                page_idx = (uint32_t)((b->offset + (page_va - b->vaddr)) / PAGE_SIZE);
                orig_prot = b->prot_flags;
            }
            break;
        }
    }
    spin_unlock_irq(&cell->lock);

    if (vmo == NULL) {
        return -EACCES;
    }

    rc = nos_cell_charge_mem(cell, PAGE_SIZE);
    if (rc == 0) {
        rc = nos_vmo_cow_break_page(vmo, page_idx, &new_pa);
        if (rc == 0) {
            rc = hal_vm_map(&cell->vm_space, page_va, new_pa, PAGE_SIZE, orig_prot);
        }
        if (rc != 0) {
            nos_cell_uncharge_mem(cell, PAGE_SIZE);
        }
    }

    (void)nos_object_put(&vmo->header);
    return rc;
}

int nos_cell_handle_page_fault(struct nos_cell *cell,
                               uintptr_t fault_va,
                               bool is_write,
                               uint32_t dfsc)
{
    uint32_t fault_class = dfsc & 0x3CU;
    struct nos_vmo *vmo = NULL;
    uintptr_t page_va = 0U;
    uint32_t page_idx = 0U;
    uint32_t prot_flags = 0U;
    uint32_t vmo_flags = 0U;
    uintptr_t pa = 0U;
    bool newly_alloc = false;
    bool charged = false;
    uint32_t i;
    int rc;

    if (cell == NULL) {
        return -EINVAL;
    }

    /* Permission Fault (0x0C..0x0F) 且为写操作：进入硬件 CoW 分裂快路 */
    if (fault_class == 0x0CU && is_write) {
        return nos_cell_handle_cow_fault(cell, fault_va);
    }

    /* Translation Fault (0x04..0x07)：为 LAZY_ALLOC 与 RECOMPUTABLE 绑定页执行按需提交 */
    if (fault_class != 0x04U) {
        return -EFAULT;
    }

    spin_lock_irq(&cell->lock);
    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        struct nos_cell_vmo_binding *b = &cell->vmo_bindings[i];

        if (!b->active || b->vmo == NULL) {
            continue;
        }
        if (fault_va >= b->vaddr && fault_va < (b->vaddr + b->len)) {
            if ((b->vmo->flags & (NOS_VMO_FLAG_LAZY_ALLOC | NOS_VMO_FLAG_RECOMPUTABLE)) != 0U &&
                (!is_write || (b->prot_flags & HAL_VM_PROT_WRITE) != 0U) &&
                nos_object_get(&b->vmo->header)) {
                vmo = b->vmo;
                vmo_flags = b->vmo->flags;
                page_va = fault_va & ~(uintptr_t)(PAGE_SIZE - 1U);
                page_idx = (uint32_t)((b->offset + (page_va - b->vaddr)) / PAGE_SIZE);
                prot_flags = b->prot_flags;
            }
            break;
        }
    }
    spin_unlock_irq(&cell->lock);

    if (vmo == NULL) {
        return -EACCES;
    }

    if ((vmo_flags & NOS_VMO_FLAG_LAZY_ALLOC) != 0U) {
        rc = nos_cell_charge_mem(cell, PAGE_SIZE);
        if (rc != 0) {
            (void)nos_object_put(&vmo->header);
            return rc;
        }
        charged = true;
    }

    rc = nos_vmo_ensure_page(vmo, page_idx, &pa, &newly_alloc);
    if (rc == 0) {
        rc = hal_vm_map(&cell->vm_space, page_va, pa, PAGE_SIZE, prot_flags);
    }

    if (charged && (rc != 0 || !newly_alloc)) {
        nos_cell_uncharge_mem(cell, PAGE_SIZE);
    }

    (void)nos_object_put(&vmo->header);
    return rc;
}

int nos_cell_rollback(struct nos_cell *cell, struct nos_cell_snapshot *snap)
{
    struct {
        uintptr_t       vaddr;
        size_t          len;
        struct nos_vmo *vmo;
    } branch_unmaps[NOS_CELL_MAX_TRACKED_VMOS];
    uint32_t branch_cnt = 0U;
    uint32_t i;

    if (cell == NULL || snap == NULL || snap->owner_cell != cell) {
        return -EINVAL;
    }

    spin_lock_irq(&cell->lock);
    if (cell->state == NOS_CELL_STATE_DESTROYED) {
        spin_unlock_irq(&cell->lock);
        return -ESTALE;
    }
    for (i = 0U; i < NOS_CELL_MAX_TRACKED_VMOS; i++) {
        struct nos_cell_vmo_binding *b = &cell->vmo_bindings[i];
        if (b->active && (snap->saved_binding_mask & (1ULL << i)) == 0ULL) {
            branch_unmaps[branch_cnt].vaddr = b->vaddr;
            branch_unmaps[branch_cnt].len = b->len;
            branch_unmaps[branch_cnt].vmo = b->vmo;
            branch_cnt++;
            b->active = false;
            b->vmo = NULL;
        }
    }
    spin_unlock_irq(&cell->lock);

    for (i = 0U; i < branch_cnt; i++) {
        (void)hal_vm_unmap(&cell->vm_space, branch_unmaps[i].vaddr, branch_unmaps[i].len);
        if (branch_unmaps[i].vmo != NULL) {
            (void)nos_object_put(&branch_unmaps[i].vmo->header);
        }
    }

    for (i = 0U; i < snap->item_count; i++) {
        struct nos_cell_snapshot_item *it = &snap->items[i];
        int rc;

        rc = nos_vmo_restore_from_snapshot(it->live_vmo, it->frozen_vmo);
        if (rc != 0) {
            return rc;
        }

        /* 恢复时继续以只读降权武装 CoW，确保同一快照可支持多次投机重试回滚而不污染 frozen 页 */
        rc = nos_vmo_remap_range(it->live_vmo,
                                 &cell->vm_space,
                                 it->vaddr,
                                 it->offset,
                                 it->len,
                                 it->prot_flags & ~HAL_VM_PROT_WRITE);
        if (rc != 0) {
            return rc;
        }
    }

    if (snap->cap_snap != NULL) {
        int rc = nos_cap_table_restore_ex(&cell->cap_table, snap->cap_snap, snap);
        if (rc != 0) {
            return rc;
        }
    }

    spin_lock_irq(&cell->lock);
    if (cell->state == NOS_CELL_STATE_DESTROYED) {
        spin_unlock_irq(&cell->lock);
        return -ESTALE;
    }
    cell->state = snap->saved_state;
    cell->cpu_ns_used = snap->saved_cpu_ns_used;
    cell->mem_bytes_used = snap->saved_mem_bytes_used;
    cell->tool_calls_used = snap->saved_tool_calls_used;
    cell->accel_ops_used = snap->saved_accel_ops_used;
    cell_append_audit_locked(cell,
                             NOS_AUDIT_CELL_ROLLBACK,
                             (uint64_t)(uintptr_t)snap,
                             0U,
                             0,
                             (uint64_t)snap->item_count,
                             cell->mem_bytes_used);
    cell_sync_telemetry_locked(cell);
    spin_unlock_irq(&cell->lock);

    return 0;
}

int nos_cell_snapshot_destroy(struct nos_cell_snapshot *snap)
{
    int rc;

    if (snap == NULL) {
        return -EINVAL;
    }

    (void)nos_object_close(&snap->header);
    rc = nos_object_retire(&snap->header);
    return (rc == -EINVAL) ? 0 : rc;
}
