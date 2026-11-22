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

static void cell_notify_budget_hit(struct nos_notification *notif)
{
    if (notif != NULL) {
        (void)nos_notify_signal_obj(notif, 1ULL);
        (void)nos_object_put(&notif->object);
    }
}

void nos_cell_subsystem_init(void)
{
    if (__atomic_exchange_n(&g_cell_subsys_inited, 1U, __ATOMIC_ACQ_REL) == 0U) {
        nos_cap_register_audit_hook(cell_cap_audit_hook);
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
            cell_notify_budget_hit(notif);
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
        cell_notify_budget_hit(notif);
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
        cell_notify_budget_hit(notif);
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
        cell_notify_budget_hit(notif);
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
        cell_notify_budget_hit(notif);
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
