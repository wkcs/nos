/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：Agent Cell 五维硬预算、EL0 只读 Telemetry 页、64B 环形审计与 CDT 联动
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/ai.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <nos/cap.h>
#include <nos/cell.h>
#include <nos/vmo.h>
#include <nos/notification.h>

_Static_assert(sizeof(struct nos_cell_telemetry) == 96,
               "nos_cell_telemetry ABI 尺寸必须为 96 字节");
_Static_assert(sizeof(struct nos_audit_record) == 64,
               "nos_audit_record ABI 尺寸必须为 64 字节");

uintptr_t kernel_running = 1;

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

static uint64_t g_mock_mono_ns = 1000000ULL;
static uint32_t g_last_map_prot = 0U;
static uintptr_t g_last_map_va = 0U;
static uintptr_t g_last_map_pa = 0U;
static size_t g_last_map_size = 0U;
static uint64_t g_notif_signals = 0ULL;
static uint32_t g_vm_switch_count = 0U;
static struct nos_vm_space *g_last_switched_space = NULL;

struct mock_va_slot {
    bool      active;
    uintptr_t vaddr;
    uintptr_t paddr;
    uint32_t  prot;
};
static struct mock_va_slot g_mock_ptes[32];

static void mock_pte_clear(void)
{
    memset(g_mock_ptes, 0, sizeof(g_mock_ptes));
}

static struct mock_va_slot *mock_pte_find(uintptr_t vaddr)
{
    for (uint32_t i = 0U; i < 32U; i++) {
        if (g_mock_ptes[i].active && g_mock_ptes[i].vaddr == vaddr) {
            return &g_mock_ptes[i];
        }
    }
    return NULL;
}

uint64_t hal_timer_monotonic_ns(void)
{
    g_mock_mono_ns += 1000ULL;
    return g_mock_mono_ns;
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    if (space == NULL) {
        return -EINVAL;
    }
    memset(space, 0, sizeof(*space));
    space->initialized = true;
    space->asid = 1U;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL) {
        space->initialized = false;
    }
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    struct mock_va_slot *s;
    (void)space;
    g_last_map_va = vaddr;
    g_last_map_pa = paddr;
    g_last_map_size = size;
    g_last_map_prot = prot_flags;
    s = mock_pte_find(vaddr);
    if (s == NULL) {
        for (uint32_t i = 0U; i < 32U; i++) {
            if (!g_mock_ptes[i].active) {
                s = &g_mock_ptes[i];
                break;
            }
        }
    }
    if (s != NULL) {
        s->active = true;
        s->vaddr = vaddr;
        s->paddr = paddr;
        s->prot = prot_flags;
    }
    return 0;
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    (void)space;
    for (uint32_t i = 0U; i < 32U; i++) {
        if (g_mock_ptes[i].active &&
            g_mock_ptes[i].vaddr >= vaddr &&
            g_mock_ptes[i].vaddr < (vaddr + size)) {
            g_mock_ptes[i].active = false;
        }
    }
    return 0;
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    g_vm_switch_count++;
    g_last_switched_space = next_space;
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    (void)notif;
    g_notif_signals += bits;
    return 0;
}

struct task_struct *task_current(void)
{
    return NULL;
}

static void test_cell_hierarchy_and_telemetry(void)
{
    struct nos_cell_budget sup_budget = {
        .cpu_ns_limit = 5000000ULL,
        .mem_bytes_limit = 65536ULL,
        .tool_calls_limit = 10ULL,
        .accel_ops_limit = 5ULL,
        .max_child_cells = 2U,
        .flags = 0U,
    };
    struct nos_cell_budget naughty_req = {
        .cpu_ns_limit = 0ULL,           /* 企图逃逸为无限 */
        .mem_bytes_limit = 128000ULL,   /* 超出父上限 */
        .tool_calls_limit = 4ULL,
        .accel_ops_limit = 2ULL,
        .max_child_cells = 10U,         /* 超出父子数量上限 */
        .flags = 0U,
    };
    struct nos_cell *sup;
    struct nos_cell *c1;
    struct nos_cell *c2;
    struct nos_cell *c3;
    struct nos_cell_budget c1_eff;
    const struct nos_cell_telemetry *tm;
    struct nos_audit_record recs[8];
    int count;

    g_notif_signals = 0ULL;
    sup = nos_cell_create(NULL, &sup_budget);
    assert(sup != NULL);

    tm = nos_cell_telemetry_view(sup);
    assert(tm != NULL);
    assert(tm->magic == NOS_CELL_TELEMETRY_MAGIC);
    assert(tm->version == 1U);
    assert(tm->state == NOS_CELL_STATE_ACTIVE);
    assert(tm->limit.mem_bytes_limit == 65536ULL);

    /* Telemetry 页映射必须严格为仅用户可读 (R--)，绝无 WRITE/EXEC */
    assert(nos_cell_map_telemetry(sup, 0x40000000UL) == 0);
    assert(g_last_map_va == 0x40000000UL);
    assert(g_last_map_size == 4096U);
    assert(g_last_map_prot == (HAL_VM_PROT_USER | HAL_VM_PROT_READ));

    /* 单调收紧验证 */
    c1 = nos_cell_create(sup, &naughty_req);
    assert(c1 != NULL);
    assert(nos_cell_get_budget(c1, &c1_eff) == 0);
    assert(c1_eff.cpu_ns_limit == 5000000ULL);
    assert(c1_eff.mem_bytes_limit == 65536ULL);
    assert(c1_eff.tool_calls_limit == 4ULL);
    assert(c1_eff.accel_ops_limit == 2ULL);
    assert(c1_eff.max_child_cells == 2U);
    assert(tm->child_cells_used == 1U);

    c2 = nos_cell_create(sup, &naughty_req);
    assert(c2 != NULL);
    assert(tm->child_cells_used == 2U);

    /* 第 3 个子 Cell 突破 max_child_cells=2 限制，应失败并审计 NOS_AUDIT_BUDGET_HIT */
    c3 = nos_cell_create(sup, &naughty_req);
    assert(c3 == NULL);
    assert(tm->state == NOS_CELL_STATE_BUDGET_HIT);

    count = nos_cell_read_audit(sup, recs, 8U);
    assert(count == 1);
    assert(recs[0].event_type == NOS_AUDIT_BUDGET_HIT);
    assert(recs[0].status == -EDQUOT);

    assert(nos_cell_destroy(c2) == 0);
    assert(nos_cell_destroy(c1) == 0);
    assert(tm->child_cells_used == 0U);
    assert(nos_cell_destroy(sup) == 0);
}

static void test_cell_5d_hard_budgets_and_audit_ring(void)
{
    struct nos_cell_budget b_mem = {
        .cpu_ns_limit = 10000ULL,
        .mem_bytes_limit = 8192ULL,
        .tool_calls_limit = 2ULL,
        .accel_ops_limit = 1ULL,
        .max_child_cells = 0U,
    };
    struct nos_notification notif;
    struct nos_cell *cell;
    const struct nos_cell_telemetry *tm;
    struct nos_audit_record ring_out[NOS_CELL_AUDIT_RING_CAPACITY];
    struct nos_object dummy_obj;
    nos_handle_t h_root;
    nos_handle_t h_child;
    uint32_t revoked = 0U;
    int count;
    int i;

    memset(&notif, 0, sizeof(notif));
    (void)nos_object_init(&notif.object, true);
    g_notif_signals = 0ULL;

    /* 1. 内存超配额 + Notification */
    cell = nos_cell_create(NULL, &b_mem);
    assert(cell != NULL);
    assert(nos_cell_bind_budget_notif(cell, &notif) == 0);
    tm = nos_cell_telemetry_view(cell);

    assert(nos_cell_charge_mem(cell, 4096ULL) == 0);
    assert(tm->mem_bytes_used == 4096ULL);
    assert(nos_cell_charge_mem(cell, 8192ULL) == -EDQUOT);
    assert(tm->state == NOS_CELL_STATE_BUDGET_HIT);
    assert(g_notif_signals == 1ULL);
    nos_cell_uncharge_mem(cell, 4096ULL);
    assert(tm->mem_bytes_used == 0ULL);
    assert(nos_cell_destroy(cell) == 0);

    /* 2. Tool Call / Accel / CPU 配额 */
    g_notif_signals = 0ULL;
    cell = nos_cell_create(NULL, &b_mem);
    assert(cell != NULL);
    assert(nos_cell_bind_budget_notif(cell, &notif) == 0);
    tm = nos_cell_telemetry_view(cell);

    assert(nos_cell_charge_tool_call(cell, 0x1111ULL, 0x10U) == 0);
    assert(nos_cell_charge_tool_call(cell, 0x2222ULL, 0x10U) == 0);
    assert(tm->tool_calls_used == 2ULL);
    assert(nos_cell_charge_tool_call(cell, 0x3333ULL, 0x10U) == -EDQUOT);
    assert(g_notif_signals == 1ULL);
    assert(nos_cell_destroy(cell) == 0);

    cell = nos_cell_create(NULL, &b_mem);
    assert(cell != NULL);
    assert(nos_cell_charge_accel_op(cell, 0xAAAAULL, 0x20U) == 0);
    assert(nos_cell_charge_accel_op(cell, 0xBBBBULL, 0x20U) == -EDQUOT);
    assert(nos_cell_destroy(cell) == 0);

    cell = nos_cell_create(NULL, &b_mem);
    assert(cell != NULL);
    tm = nos_cell_telemetry_view(cell);
    assert(nos_cell_charge_cpu(cell, 6000ULL) == 0);
    assert(tm->cpu_ns_used == 6000ULL);
    assert(nos_cell_charge_cpu(cell, 5000ULL) == -EDQUOT);
    assert(tm->state == NOS_CELL_STATE_BUDGET_HIT);
    assert(nos_cell_destroy(cell) == 0);

    /* 3. 审计环 64 条回绕 + CDT 派生/吊销审计联动 */
    cell = nos_cell_create(NULL, &b_mem);
    assert(cell != NULL);
    tm = nos_cell_telemetry_view(cell);

    for (i = 0; i < 80; i++) {
        nos_cell_record_audit(cell, NOS_AUDIT_FAULT, (uint64_t)(i + 1),
                              0U, -EFAULT, (uint64_t)i, 0ULL);
    }
    assert(tm->audit_seq == 80ULL);

    count = nos_cell_read_audit(cell, ring_out, NOS_CELL_AUDIT_RING_CAPACITY);
    assert(count == (int)NOS_CELL_AUDIT_RING_CAPACITY);
    /* 最早保留项应为第 17 条 (schema_id=17)，最新项为第 80 条 */
    assert(ring_out[0].schema_id == 17ULL);
    assert(ring_out[NOS_CELL_AUDIT_RING_CAPACITY - 1U].schema_id == 80ULL);

    nos_object_init(&dummy_obj, true);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, &dummy_obj,
                         NOS_CAP_RIGHT_ALL, 0x55U, &h_root) == 0);
    assert(nos_cap_derive(nos_cell_cap_table(cell), h_root,
                          nos_cell_cap_table(cell), NOS_CAP_RIGHT_READ,
                          0U, &h_child) == 0);
    assert(nos_cap_revoke_tree(nos_cell_cap_table(cell), h_root, &revoked) == 0);
    assert(revoked == 2U);

    count = nos_cell_read_audit(cell, ring_out, NOS_CELL_AUDIT_RING_CAPACITY);
    assert(count == (int)NOS_CELL_AUDIT_RING_CAPACITY);
    assert(ring_out[count - 2].event_type == NOS_AUDIT_CAP_DERIVE);
    assert(ring_out[count - 1].event_type == NOS_AUDIT_CAP_REVOKE);
    assert(ring_out[count - 1].arg1 == 2ULL);

    /* 4. Task 附着与引用计数保护（Cell 销毁在最后一个 Task 分离后才会彻底释放） */
    struct task_struct mock_task;
    memset(&mock_task, 0, sizeof(mock_task));
    assert(nos_cell_attach_task(&mock_task, cell) == 0);
    assert(mock_task.cell == cell);
    assert(nos_task_active_vm_space(&mock_task) == nos_cell_vm_space(cell));
    assert(nos_task_active_cap_table(&mock_task) == nos_cell_cap_table(cell));
    assert(nos_cell_destroy(cell) == 0);
    /* mock_task 分离归还最后一次引用 */
    nos_cell_detach_task(&mock_task);
    assert(mock_task.cell == NULL);
}

static void test_cell_hardware_cow_snapshot_and_rollback(void)
{
    struct nos_cell_budget budget = {
        .cpu_ns_limit = 1000000ULL,
        .mem_bytes_limit = 65536ULL,
        .tool_calls_limit = 10ULL,
        .accel_ops_limit = 10ULL,
        .max_child_cells = 0U,
    };
    struct nos_vmo_create_args work_args = {
        .size_bytes = 4096U,
        .kind = NOS_VMO_ANON,
        .flags = 0U,
    };
    struct nos_vmo_create_args dma_args = {
        .size_bytes = 4096U,
        .kind = NOS_VMO_DMA_TENSOR,
        .flags = NOS_VMO_FLAG_IO_COHERENT,
    };
    struct nos_cell *cell;
    struct nos_vmo *work_vmo;
    struct nos_vmo *dma_vmo;
    struct nos_cell_snapshot *snap;
    const struct nos_cell_telemetry *tm;
    struct mock_va_slot *pte_work;
    struct mock_va_slot *pte_dma;
    struct nos_audit_record recs[16];
    uintptr_t base_pa;
    char rb[32];
    int count;
    bool seen_snap = false;
    bool seen_rb = false;

    mock_pte_clear();
    cell = nos_cell_create(NULL, &budget);
    assert(cell != NULL);
    tm = nos_cell_telemetry_view(cell);

    work_vmo = nos_vmo_create(&work_args);
    dma_vmo = nos_vmo_create(&dma_args);
    assert(work_vmo != NULL && dma_vmo != NULL);
    base_pa = work_vmo->pages[0]->paddr;
    assert(nos_vmo_write(work_vmo, 0U, "BASE_STATE_V1", 14U) == 0);

    assert(nos_vmo_map(work_vmo, nos_cell_vm_space(cell), 0x450000UL, 0U, 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, cell) == 0);
    assert(nos_vmo_map(dma_vmo, nos_cell_vm_space(cell), 0x440000UL, 0U, 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, cell) == 0);
    assert(tm->mem_bytes_used == 8192ULL);

    /* 1. 执行零拷贝硬件 CoW Snapshot：work_vmo PTE 降为只读，DMA/Ring 页保持可写 */
    snap = nos_cell_snapshot(cell);
    assert(snap != NULL);
    assert(snap->item_count == 1U);
    pte_work = mock_pte_find(0x450000UL);
    pte_dma = mock_pte_find(0x440000UL);
    assert(pte_work != NULL && (pte_work->prot & HAL_VM_PROT_WRITE) == 0U);
    assert(pte_dma != NULL && (pte_dma->prot & HAL_VM_PROT_WRITE) != 0U);

    /* 2. 对只读 Telemetry 区域调用 CoW Fault 处理必须被严格拒绝 */
    assert(nos_cell_handle_cow_fault(cell, 0x420000UL) == -EACCES);

    /* 3. 对工作区页模拟硬件 EL0 Data Abort CoW 页断裂 + 分支篡改 + 消耗工具配额 */
    assert(nos_cell_handle_cow_fault(cell, 0x450008UL) == 0);
    assert(pte_work->paddr != base_pa && (pte_work->prot & HAL_VM_PROT_WRITE) != 0U);
    assert(nos_vmo_write(work_vmo, 0U, "DIRTY_BRANCH_1", 15U) == 0);
    assert(nos_cell_charge_tool_call(cell, 0x1010ULL, 1U) == 0);
    assert(tm->tool_calls_used == 1ULL);

    /* 4. Rollback 到检查点：数据、状态、配额计数同时还原且 CoW 仍维持武装 */
    assert(nos_cell_rollback(cell, snap) == 0);
    assert(work_vmo->pages[0]->paddr == base_pa);
    memset(rb, 0, sizeof(rb));
    assert(nos_vmo_read(work_vmo, 0U, rb, 14U) == 0);
    assert(strcmp(rb, "BASE_STATE_V1") == 0);
    assert(tm->tool_calls_used == 0ULL);
    assert((pte_work->prot & HAL_VM_PROT_WRITE) == 0U);

    /* 5. 第 2 轮投机 CoW 分支写 + 回滚验证同一快照可重复使用 */
    assert(nos_cell_handle_cow_fault(cell, 0x450000UL) == 0);
    assert(nos_vmo_write(work_vmo, 0U, "DIRTY_BRANCH_2", 15U) == 0);
    assert(nos_cell_rollback(cell, snap) == 0);
    memset(rb, 0, sizeof(rb));
    assert(nos_vmo_read(work_vmo, 0U, rb, 14U) == 0);
    assert(strcmp(rb, "BASE_STATE_V1") == 0);

    count = nos_cell_read_audit(cell, recs, 16U);
    assert(count >= 2);
    for (int i = 0; i < count; i++) {
        if (recs[i].event_type == NOS_AUDIT_CELL_SNAPSHOT) {
            seen_snap = true;
        } else if (recs[i].event_type == NOS_AUDIT_CELL_ROLLBACK) {
            seen_rb = true;
        }
    }
    assert(seen_snap && seen_rb);

    assert(nos_cell_snapshot_destroy(snap) == 0);
    assert(nos_vmo_unmap(nos_cell_vm_space(cell), 0x440000UL, 4096U, cell) == 0);
    assert(nos_vmo_unmap(nos_cell_vm_space(cell), 0x450000UL, 4096U, cell) == 0);
    assert(nos_object_close(&work_vmo->header) == 0);
    assert(nos_object_retire(&work_vmo->header) == 0);
    assert(nos_object_close(&dma_vmo->header) == 0);
    assert(nos_object_retire(&dma_vmo->header) == 0);
    assert(nos_cell_destroy(cell) == 0);
}

int main(void)
{
    test_cell_hierarchy_and_telemetry();
    test_cell_5d_hard_budgets_and_audit_ring();
    test_cell_hardware_cow_snapshot_and_rollback();
    printf("PASS: test_agent_cell (5D budgets, RO telemetry, 64B audit, CDT, CoW Snapshot/Rollback)\n");
    return 0;
}
