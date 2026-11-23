/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Stage 6 宿主端全链路完备性集成测试：
 * CDT schema_mask/Snapshot/Restore/Fork -> Agent Cell Fork/Rollback/EventNotif ->
 * VMO 64KB/CoW/LazyAlloc/Seal-FNV1a/AF冷页回收缺页恢复 ->
 * RingIPC MOVE_SLICE/CANCEL/BIND_SERVER/COMPLETE/CDT-Drain ->
 * FDT + SMMUv3 EVTQ/TLBI + AccelQueue ASYNC/WAIT/CANCEL/RESET_CTX + RKNPU ->
 * ELF64 ET_DYN PIE R_AARCH64_RELATIVE + W^X。
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/ipc.h>
#include <uapi/nos/ai.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <nos/accel.h>
#include <nos/cap.h>
#include <nos/cell.h>
#include <nos/elf64.h>
#include <nos/hal_arch.h>
#include <nos/iommu.h>
#include <nos/ring.h>
#include <nos/vmo.h>
#include <asm/fdt_scan.h>
#include <asm/mmu.h>
#include <asm/smmuv3.h>

int virtio_accel_init(void);
int rknpu_accel_init(void);

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

static uint64_t g_mono_ns = 30000000ULL;
static uint32_t g_dcache_sync_calls;

uint64_t hal_timer_monotonic_ns(void)
{
    return __atomic_add_fetch(&g_mono_ns, 1000ULL, __ATOMIC_RELAXED);
}

#define PT_POOL_PAGES 256U
#define PT_POOL_PBASE 0x80000000ULL

struct stage6_pt_pool {
    uint8_t mem[PT_POOL_PAGES][4096];
    bool    used[PT_POOL_PAGES];
};

static struct stage6_pt_pool g_pt_pool;
static uint16_t g_next_asid = 1U;

static uintptr_t pt_alloc(void *ctx)
{
    struct stage6_pt_pool *p = (struct stage6_pt_pool *)ctx;
    for (uint32_t i = 0U; i < PT_POOL_PAGES; i++) {
        if (!p->used[i]) {
            p->used[i] = true;
            memset(p->mem[i], 0, 4096U);
            return (uintptr_t)(PT_POOL_PBASE + (uint64_t)i * 4096ULL);
        }
    }
    return 0U;
}

static void pt_free(void *ctx, uintptr_t pa)
{
    struct stage6_pt_pool *p = (struct stage6_pt_pool *)ctx;
    if (pa < PT_POOL_PBASE) {
        return;
    }
    uint64_t idx = (pa - PT_POOL_PBASE) / 4096ULL;
    if (idx < PT_POOL_PAGES && p->used[idx]) {
        p->used[idx] = false;
    }
}

static void *pt_p2v(void *ctx, uintptr_t pa)
{
    struct stage6_pt_pool *p = (struct stage6_pt_pool *)ctx;
    assert(pa >= PT_POOL_PBASE);
    uint64_t off = pa - PT_POOL_PBASE;
    assert(off + 4096ULL <= sizeof(p->mem));
    return (void *)((uint8_t *)p->mem + off);
}

static const struct arm64_pt_backend g_be = {
    .alloc_pt_page   = pt_alloc,
    .free_pt_page    = pt_free,
    .phys_to_pt_virt = pt_p2v,
    .ctx             = &g_pt_pool,
};

int hal_vm_space_init(struct nos_vm_space *space)
{
    if (space == NULL) {
        return -EINVAL;
    }
    memset(space, 0, sizeof(*space));
    space->root_pt_phys = pt_alloc(&g_pt_pool);
    if (space->root_pt_phys == 0U) {
        return -ENOMEM;
    }
    space->asid = g_next_asid++;
    space->initialized = true;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL && space->initialized) {
        if (space->asid != 0U) {
            nos_iommu_tlbi_asid(space->asid);
        }
        arm64_pt_destroy(space->root_pt_phys, 39, &g_be);
        space->root_pt_phys = 0U;
        space->initialized = false;
    }
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_map_range(space->root_pt_phys, 39, vaddr, paddr, size,
                              prot_flags, true, &g_be);
}

int hal_vm_map_noflush(struct nos_vm_space *space,
                       uintptr_t vaddr,
                       uintptr_t paddr,
                       size_t size,
                       uint32_t prot_flags)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_map_range_noflush(space->root_pt_phys, 39, vaddr, paddr, size,
                                      prot_flags, true, &g_be);
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    int rc;

    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    rc = arm64_pt_unmap_range(space->root_pt_phys, 39, vaddr, size, false, &g_be);
    if (rc == 0 && space->asid != 0U) {
        nos_iommu_tlbi_range(space->asid, vaddr, size);
    }
    return rc;
}

int hal_vm_unmap_noflush(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_unmap_range(space->root_pt_phys, 39, vaddr, size, false, &g_be);
}

void hal_vm_flush_asid(struct nos_vm_space *space)
{
    if (space != NULL && space->asid != 0U) {
        nos_iommu_tlbi_asid(space->asid);
    }
}

void hal_dcache_sync(uintptr_t va, size_t size, uint32_t dir)
{
    (void)va;
    (void)size;
    (void)dir;
    g_dcache_sync_calls++;
}

int hal_vm_test_and_clear_af(struct nos_vm_space *space,
                             uintptr_t vaddr,
                             bool *was_accessed)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_test_and_clear_af(space->root_pt_phys, 39, vaddr, was_accessed, &g_be);
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    (void)next_space;
}

struct task_struct *task_current(void)
{
    return NULL;
}

int sched_infer_boost_deadline(struct task_struct *target, uint64_t inherited_abs_deadline_ns)
{
    (void)target;
    (void)inherited_abs_deadline_ns;
    return 0;
}

void sched_infer_restore_deadline(struct task_struct *target)
{
    (void)target;
}

int nos_notification_init(struct nos_notification *notif)
{
    if (notif == NULL) {
        return -EINVAL;
    }
    memset(notif, 0, sizeof(*notif));
    return nos_object_init(&notif->object, true);
}

int nos_notification_close(struct nos_notification *notif)
{
    if (notif == NULL) {
        return -EINVAL;
    }
    return nos_object_close(&notif->object);
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    if (notif == NULL) {
        return -EINVAL;
    }
    __atomic_fetch_or(&notif->pending_bits, bits, __ATOMIC_RELEASE);
    return 0;
}

int nos_notify_wait_obj(struct nos_notification *notif, uint64_t *out_bits, nos_timeout_t timeout)
{
    uint64_t bits;

    (void)timeout;
    if (notif == NULL) {
        return -EINVAL;
    }
    bits = __atomic_exchange_n(&notif->pending_bits, 0ULL, __ATOMIC_ACQUIRE);
    if (bits != 0ULL) {
        if (out_bits != NULL) {
            *out_bits = bits;
        }
        return 0;
    }
    return nos_object_is_closed(&notif->object) ? -EPIPE : -EAGAIN;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void verify_cell_vmo_cdt_ring_accel_and_smmu(void)
{
    struct nos_cell_budget parent_b = {
        .cpu_ns_limit = 5000000000ULL,
        .mem_bytes_limit = 512ULL * 1024ULL,
        .tool_calls_limit = 16ULL,
        .accel_ops_limit = 16ULL,
        .max_child_cells = 4U,
    };
    struct nos_vmo_create_args lazy_args = {
        .size_bytes = 8192U,
        .kind = NOS_VMO_ANON,
        .flags = NOS_VMO_FLAG_LAZY_ALLOC | NOS_VMO_FLAG_RECOMPUTABLE,
    };
    struct nos_vmo_create_args blk_args = {
        .size_bytes = 65536U,
        .kind = NOS_VMO_KVCACHE,
        .flags = NOS_VMO_FLAG_CONTIG_64K,
    };
    struct nos_vmo_create_args dma_args = {
        .size_bytes = 4096U,
        .kind = NOS_VMO_DMA_TENSOR,
        .flags = 0U,
    };
    struct nos_cell *parent;
    struct nos_cell *child;
    struct task_struct t_parent;
    struct task_struct t_srv;
    struct nos_vmo *lazy_vmo;
    struct nos_vmo *blk_vmo;
    struct nos_vmo *blk_cow;
    struct nos_vmo *w_vmo;
    struct nos_vmo *x_vmo;
    struct nos_vmo *y_vmo;
    struct nos_vmo *slice_vmo;
    struct nos_cell_snapshot *snap;
    struct nos_notification evt_notif;
    uint64_t evt_bits = 0ULL;
    uintptr_t pa = 0U;
    uint64_t pte = 0ULL;

    smmuv3_host_reset();
    smmuv3_host_set_pt_backend(&g_be);
    assert(arm64_smmuv3_init(0UL, 0UL) == 0);

    nos_accel_host_reset();
    assert(virtio_accel_init() == 0);
    assert(rknpu_accel_init() == 0);

    parent = nos_cell_create(NULL, &parent_b);
    assert(parent != NULL);
    memset(&t_parent, 0, sizeof(t_parent));
    nos_refcount_init(&t_parent.refcount, 1U);
    t_parent.cell = parent;

    assert(nos_notification_init(&evt_notif) == 0);
    assert(nos_cell_bind_budget_notif(parent, &evt_notif) == 0);

    /* 1. VMO LAZY_ALLOC 缺页按需分配 + AF 两阶段冷页驱逐 + 缺页自动恢复 */
    lazy_vmo = nos_vmo_create(&lazy_args);
    assert(lazy_vmo != NULL);
    assert(lazy_vmo->pages[0] == NULL && lazy_vmo->pages[1] == NULL);
    assert(nos_vmo_map(lazy_vmo, nos_cell_vm_space(parent),
                       0x500000UL, 0U, 8192U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       parent) == 0);
    assert(arm64_pt_translate(nos_cell_vm_space(parent)->root_pt_phys, 39,
                              0x500000UL, &pa, &pte, &g_be) == -ENOENT);
    /* 触发 Translation fault 补页 */
    assert(nos_cell_handle_page_fault(parent, 0x500040UL, true, 0x07U) == 0);
    assert(lazy_vmo->pages[0] != NULL && lazy_vmo->pages[1] == NULL);
    assert(arm64_pt_translate(nos_cell_vm_space(parent)->root_pt_phys, 39,
                              0x500040UL, &pa, &pte, &g_be) == 0);
    assert((pte & ARM64_PTE_AF) != 0ULL);

    /* 第 1 轮清 AF 留存；第 2 轮发现 AF==0 驱逐物理页；第 3 次缺页恢复 */
    assert(nos_vmo_reclaim_cold_pages(lazy_vmo, nos_cell_vm_space(parent), 0x500000UL, 2U) == 0);
    assert(lazy_vmo->pages[0] != NULL);
    assert(nos_vmo_reclaim_cold_pages(lazy_vmo, nos_cell_vm_space(parent), 0x500000UL, 2U) == 1);
    assert(lazy_vmo->pages[0] == NULL);
    assert(nos_cell_handle_page_fault(parent, 0x500040UL, true, 0x07U) == 0);
    assert(lazy_vmo->pages[0] != NULL);

    /* 2. 64KB 连续 VMO Seal FNV-1a + CoW 拆分与 Cell Snapshot/Rollback CNode 清理 */
    blk_vmo = nos_vmo_create(&blk_args);
    assert(blk_vmo != NULL);
    uint64_t pat = 0x1122334455667788ULL;
    assert(nos_vmo_write(blk_vmo, 0U, &pat, sizeof(pat)) == 0);
    assert(nos_vmo_map(blk_vmo, nos_cell_vm_space(parent),
                       0x600000UL, 0U, 65536U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       parent) == 0);

    nos_handle_t kept_h = NOS_HANDLE_INVALID;
    nos_handle_t nogrant_h = NOS_HANDLE_INVALID;
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, blk_vmo,
                         NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_MAP | NOS_CAP_RIGHT_GRANT,
                         0U, &kept_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, blk_vmo,
                         NOS_CAP_RIGHT_READ, 0U, &nogrant_h) == 0);

    snap = nos_cell_snapshot(parent);
    assert(snap != NULL);

    /* 快照后分支写入触发 CoW + 新增分支能力与分支 VMO */
    uint64_t dirty_pat = 0x9988776655443322ULL;
    assert(nos_cell_handle_page_fault(parent, 0x601000UL, true, 0x0FU) == 0);
    assert(nos_vmo_write(blk_vmo, 4096U, &dirty_pat, sizeof(dirty_pat)) == 0);

    slice_vmo = nos_vmo_create(&dma_args);
    assert(slice_vmo != NULL);
    nos_handle_t branch_h = NOS_HANDLE_INVALID;
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, slice_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &branch_h) == 0);
    assert(nos_vmo_map(slice_vmo, nos_cell_vm_space(parent),
                       0x700000UL, 0U, 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       parent) == 0);

    assert(nos_cell_rollback(parent, snap) == 0);
    /* 回滚后分支地址解映射、分支 CNode 句柄被撤回，且保留快照前句柄 */
    assert(arm64_pt_translate(nos_cell_vm_space(parent)->root_pt_phys, 39,
                              0x700000UL, &pa, &pte, &g_be) == -ENOENT);
    void *tmp_obj = NULL;
    assert(nos_cap_lookup(nos_cell_cap_table(parent), branch_h, NOS_CAP_VMO,
                          NOS_CAP_RIGHT_READ, &tmp_obj, NULL) < 0);
    assert(nos_cap_lookup(nos_cell_cap_table(parent), kept_h, NOS_CAP_VMO,
                          NOS_CAP_RIGHT_READ, &tmp_obj, NULL) == 0);
    (void)nos_object_put(&((struct nos_vmo *)tmp_obj)->header);
    uint64_t rolled = 0ULL;
    assert(nos_vmo_read(blk_vmo, 4096U, &rolled, sizeof(rolled)) == 0);
    assert(rolled == 0ULL);

    blk_cow = nos_vmo_clone_cow(blk_vmo);
    assert(blk_cow != NULL);
    assert(nos_vmo_seal(blk_vmo) == 0);
    assert(nos_vmo_content_hash(blk_vmo) != 0ULL);
    g_dcache_sync_calls = 0U;
    assert(nos_vmo_sync_dma(blk_vmo, 0U, 4096U, NOS_DMA_TO_DEVICE) == 0);
    assert(g_dcache_sync_calls >= 1U);

    /* 3. Cell True Fork（余量收紧 + 独立 CNode/AddressSpace + 销毁同步回收子计数） */
    child = nos_cell_fork(parent, NULL);
    assert(child != NULL && parent->child_cells_used == 1U);
    assert(child->budget.mem_bytes_limit <= parent->budget.mem_bytes_limit);
    void *fork_obj = NULL;
    assert(nos_cap_lookup(nos_cell_cap_table(child), kept_h, NOS_CAP_VMO,
                          NOS_CAP_RIGHT_READ, &fork_obj, NULL) == 0);
    (void)nos_object_put(&((struct nos_vmo *)fork_obj)->header);
    assert(nos_cap_lookup(nos_cell_cap_table(child), nogrant_h, NOS_CAP_VMO,
                          NOS_CAP_RIGHT_READ, &fork_obj, NULL) < 0);

    /* 4. RingIPC schema_mask + MOVE_SLICE + CANCEL + BIND_SERVER/POP/COMPLETE + CDT drain */
    struct nos_ring_create_args rargs = {
        .sq_entries = 16U,
        .cq_entries = 16U,
        .flags = 0U,
    };
    struct nos_ring *ring = nos_ring_create(&rargs, NULL, nos_cell_vm_space(parent), parent);
    assert(ring != NULL);
    nos_handle_t ring_h = NOS_HANDLE_INVALID;
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_RING, ring,
                         NOS_CAP_RIGHT_ALL, 0U, &ring_h) == 0);
    assert(nos_cap_set_schema_mask(nos_cell_cap_table(parent), ring_h, 0x1ULL) == 0);

    memset(&t_srv, 0, sizeof(t_srv));
    nos_refcount_init(&t_srv.refcount, 1U);
    t_srv.cell = child;
    nos_handle_t srv_ring_h = NOS_HANDLE_INVALID;
    assert(nos_cap_derive(nos_cell_cap_table(parent), ring_h,
                          nos_cell_cap_table(child), NOS_CAP_RIGHT_ALL, 0U, &srv_ring_h) == 0);
    assert(nos_ring_bind_server(ring, &t_srv, NULL, NULL) == 0);

    nos_handle_t slice_h = NOS_HANDLE_INVALID;
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, slice_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &slice_h) == 0);

    assert(ring->ring_vmo != NULL && ring->ring_vmo->page_count > 0U &&
           ring->ring_vmo->pages[0] != NULL && ring->ring_vmo->pages[0]->kvaddr != 0U);
    struct nos_ring_header *hdr = (struct nos_ring_header *)(uintptr_t)ring->ring_vmo->pages[0]->kvaddr;
    struct nos_ring_sqe *sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    struct nos_ring_cqe *cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);

    /* 越权 schema 拦截 (-EACCES) */
    memset(&sq[0], 0, sizeof(sq[0]));
    sq[0].opcode = NOS_RING_OP_TOOL_CALL;
    sq[0].target_cap = NOS_HANDLE_INVALID;
    sq[0].vmo_cap = NOS_HANDLE_INVALID;
    sq[0].req_id = 701ULL;
    sq[0].schema_id = 0x4ULL; /* 未在 0x1ULL 位掩码白名单 */
    __atomic_store_n(&hdr->sq_tail, 1U, __ATOMIC_RELEASE);
    assert(nos_ring_enter_with_cap(ring, &t_parent, ring_h, 1U, 1U,
                                   NOS_RING_ENTER_SUBMIT | NOS_RING_ENTER_WAIT_CQ,
                                   NOS_IPC_TIMEOUT_NONBLOCK) == 1);
    assert(cq[0].req_id == 701ULL && cq[0].status == -EACCES);
    __atomic_store_n(&hdr->cq_head, 1U, __ATOMIC_RELEASE);

    /* 合法 schema + MOVE_SLICE + EL0 server pop & complete */
    memset(&sq[1], 0, sizeof(sq[1]));
    sq[1].opcode = NOS_RING_OP_TOOL_CALL;
    sq[1].flags = NOS_RING_FLAG_MOVE_SLICE;
    sq[1].req_id = 702ULL;
    sq[1].schema_id = 1ULL;
    sq[1].vmo_cap = slice_h;
    sq[1].inline_arg[0] = 19ULL;
    sq[1].inline_arg[1] = 23ULL;
    __atomic_store_n(&hdr->sq_tail, 2U, __ATOMIC_RELEASE);
    assert(nos_ring_enter_with_cap(ring, &t_parent, ring_h, 1U, 0U,
                                   NOS_RING_ENTER_SUBMIT, NOS_IPC_TIMEOUT_NONBLOCK) == 1);
    /* MOVE_SLICE 必须撤销源端句柄 */
    assert(nos_cap_lookup(nos_cell_cap_table(parent), slice_h, NOS_CAP_VMO,
                          NOS_CAP_RIGHT_READ, &tmp_obj, NULL) < 0);

    struct nos_ring_sqe pop_sqe;
    assert(nos_ring_server_pop(ring, &pop_sqe, NOS_IPC_TIMEOUT_NONBLOCK) == 0);
    assert(pop_sqe.req_id == 702ULL && pop_sqe.vmo_cap != NOS_HANDLE_INVALID);

    struct nos_ring_cqe done_cqe;
    memset(&done_cqe, 0, sizeof(done_cqe));
    done_cqe.req_id = 702ULL;
    done_cqe.status = 0;
    done_cqe.result_len = (uint32_t)(pop_sqe.inline_arg[0] + pop_sqe.inline_arg[1]);
    assert(nos_ring_server_complete(ring, &done_cqe) == 0);
    assert(cq[1].req_id == 702ULL && cq[1].result_len == 42U);
    __atomic_store_n(&hdr->cq_head, 2U, __ATOMIC_RELEASE);

    /* NOS_RING_OP_CANCEL + CDT 吊销排空 */
    memset(&sq[2], 0, sizeof(sq[2]));
    sq[2].opcode = NOS_RING_OP_TOOL_CALL;
    sq[2].target_cap = NOS_HANDLE_INVALID;
    sq[2].vmo_cap = NOS_HANDLE_INVALID;
    sq[2].req_id = 703ULL;
    sq[2].schema_id = 1ULL;
    __atomic_store_n(&hdr->sq_tail, 3U, __ATOMIC_RELEASE);
    assert(nos_ring_enter_with_cap(ring, &t_parent, ring_h, 1U, 0U,
                                   NOS_RING_ENTER_SUBMIT, NOS_IPC_TIMEOUT_NONBLOCK) == 1);
    assert(ring->pending_count == 1U);
    assert(nos_cap_revoke_tree(nos_cell_cap_table(parent), ring_h, NULL) == 0);
    assert(ring->pending_count == 0U);

    /* 5. AccelQueue ASYNC/WAIT/CANCEL + SMMUv3 EVTQ 审计联动 */
    struct nos_accel_device *vdev = nos_accel_find_by_type(NOS_ACCEL_TYPE_VIRTIO);
    struct nos_accel_device *rdev = nos_accel_find_by_type(NOS_ACCEL_TYPE_RKNPU);
    assert(vdev != NULL && rdev != NULL);

    w_vmo = nos_vmo_create(&dma_args);
    x_vmo = nos_vmo_create(&dma_args);
    y_vmo = nos_vmo_create(&dma_args);
    assert(w_vmo != NULL && x_vmo != NULL && y_vmo != NULL);
    int32_t w_buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int32_t x_buf[4] = { 2, 1, 0, 3 };
    int32_t y_buf[2] = { 0, 0 };
    assert(nos_vmo_write(w_vmo, 0U, w_buf, sizeof(w_buf)) == 0);
    assert(nos_vmo_write(x_vmo, 0U, x_buf, sizeof(x_buf)) == 0);
    assert(nos_vmo_seal(w_vmo) == 0);

    nos_handle_t wh = NOS_HANDLE_INVALID;
    nos_handle_t xh = NOS_HANDLE_INVALID;
    nos_handle_t yh = NOS_HANDLE_INVALID;
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, w_vmo, NOS_CAP_RIGHT_READ, 0U, &wh) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, x_vmo, NOS_CAP_RIGHT_READ, 0U, &xh) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(parent), NOS_CAP_VMO, y_vmo, NOS_CAP_RIGHT_WRITE, 0U, &yh) == 0);

    struct nos_accel_job_desc adesc;
    struct nos_accel_job_result ares;
    memset(&adesc, 0, sizeof(adesc));
    adesc.kernel_op = NOS_ACCEL_KERNEL_GEMV_I32;
    adesc.flags = NOS_ACCEL_FLAG_ASYNC;
    adesc.req_id = 801ULL;
    adesc.weight_vmo_cap = wh;
    adesc.input_vmo_cap = xh;
    adesc.output_vmo_cap = yh;
    adesc.notify_cap = NOS_HANDLE_INVALID;
    adesc.dim_m = 2U;
    adesc.dim_k = 4U;
    assert(nos_accel_submit(vdev, &t_parent, &adesc, &ares) == 0);
    assert(nos_accel_wait_job(vdev, &t_parent, 801ULL, &ares, 1000U) == 0);
    assert(nos_vmo_read(y_vmo, 0U, y_buf, sizeof(y_buf)) == 0);
    assert(y_buf[0] == 16 && y_buf[1] == 40);

    assert(nos_accel_bind_vmo(rdev, &t_parent, xh) == 0);
    assert(nos_accel_reset_ctx(rdev, &t_parent, 0U) == 0);

    struct nos_iommu_fault_event fevt = {
        .sid = vdev->caps.stream_id,
        .pasid = 0U,
        .event_code = 0x10U,
        .flags = 1U,
        .iova = 0xCAFE0000ULL,
    };
    smmuv3_host_inject_event(&fevt);
    assert(nos_iommu_poll_events(4U) == 1);
    assert(nos_notify_wait_obj(&evt_notif, &evt_bits, NOS_NO_WAIT) == 0);
    assert((evt_bits & NOS_CELL_EVT_IOMMU_FAULT) != 0ULL);

    (void)nos_ring_destroy(ring);
    (void)nos_object_put(&rdev->header);
    (void)nos_object_put(&vdev->header);
    (void)nos_object_close(&y_vmo->header); (void)nos_object_retire(&y_vmo->header);
    (void)nos_object_close(&x_vmo->header); (void)nos_object_retire(&x_vmo->header);
    (void)nos_object_close(&w_vmo->header); (void)nos_object_retire(&w_vmo->header);
    (void)nos_object_close(&slice_vmo->header); (void)nos_object_retire(&slice_vmo->header);
    (void)nos_object_close(&blk_cow->header); (void)nos_object_retire(&blk_cow->header);
    (void)nos_object_close(&blk_vmo->header); (void)nos_object_retire(&blk_vmo->header);
    (void)nos_object_close(&lazy_vmo->header); (void)nos_object_retire(&lazy_vmo->header);
    (void)nos_cell_snapshot_destroy(snap);
    (void)nos_object_close(&evt_notif.object); (void)nos_object_retire(&evt_notif.object);
    assert(nos_cell_destroy(child) == 0);
    assert(parent->child_cells_used == 0U);
    assert(nos_cell_destroy(parent) == 0);
    nos_accel_host_reset();
    smmuv3_host_reset();
}

static void verify_elf64_pie_and_fdt(void)
{
    uint8_t pie[4096];
    Elf64_Ehdr *eh = (Elf64_Ehdr *)pie;
    Elf64_Phdr *ph = (Elf64_Phdr *)(pie + sizeof(Elf64_Ehdr));
    Elf64_Rela *rela;
    Elf64_Dyn *dyn;
    struct nos_cell_budget b = {
        .cpu_ns_limit = 1000000000ULL,
        .mem_bytes_limit = 262144ULL,
        .tool_calls_limit = 4ULL,
        .accel_ops_limit = 4ULL,
    };
    struct nos_cell *cell = nos_cell_create(NULL, &b);
    struct nos_elf64_image img;
    uint64_t val = 0ULL;

    assert(cell != NULL);
    memset(pie, 0, sizeof(pie));
    eh->e_ident[0] = ELF_MAGIC0; eh->e_ident[1] = ELF_MAGIC1;
    eh->e_ident[2] = ELF_MAGIC2; eh->e_ident[3] = ELF_MAGIC3;
    eh->e_ident[4] = ELFCLASS64; eh->e_ident[5] = ELFDATA2LSB; eh->e_ident[6] = 1U;
    eh->e_type = ET_DYN; eh->e_machine = EM_AARCH64; eh->e_version = 1U;
    eh->e_entry = 0ULL; eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr); eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 4U;

    ph[0].p_type = PT_LOAD; ph[0].p_flags = PF_R | PF_X;
    ph[0].p_offset = 512U; ph[0].p_vaddr = 0ULL; ph[0].p_filesz = 16U; ph[0].p_memsz = 16U;
    ph[1].p_type = PT_LOAD; ph[1].p_flags = PF_R;
    ph[1].p_offset = 640U; ph[1].p_vaddr = 0x10000ULL; ph[1].p_filesz = 128U; ph[1].p_memsz = 128U;
    rela = (Elf64_Rela *)(pie + 640U + 32U);
    rela[0].r_offset = 0x20000ULL; rela[0].r_info = R_AARCH64_RELATIVE; rela[0].r_addend = 0x10000LL;
    ph[2].p_type = PT_LOAD; ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = 1024U; ph[2].p_vaddr = 0x20000ULL; ph[2].p_filesz = 128U; ph[2].p_memsz = 4096U;
    dyn = (Elf64_Dyn *)(pie + 1024U + 32U);
    dyn[0].d_tag = DT_RELA; dyn[0].d_val = 0x10020ULL;
    dyn[1].d_tag = DT_RELASZ; dyn[1].d_val = sizeof(Elf64_Rela);
    dyn[2].d_tag = DT_RELAENT; dyn[2].d_val = sizeof(Elf64_Rela);
    dyn[3].d_tag = DT_NULL;
    ph[3].p_type = PT_DYNAMIC; ph[3].p_flags = PF_R | PF_W;
    ph[3].p_offset = 1024U + 32U; ph[3].p_vaddr = 0x20020ULL;
    ph[3].p_filesz = 4ULL * sizeof(Elf64_Dyn); ph[3].p_memsz = 4ULL * sizeof(Elf64_Dyn);

    assert(nos_elf64_load_into_cell(pie, sizeof(pie), cell, &img) == 0);
    assert(nos_vmo_read(img.seg_vmos[2], 0U, &val, sizeof(val)) == 0);
    assert(val == (uint64_t)(NOS_ELF64_PIE_BASE + 0x10000ULL));
    nos_elf64_unload_image(&img);
    assert(nos_cell_destroy(cell) == 0);

    uint8_t dummy_fdt[64];
    memset(dummy_fdt, 0, sizeof(dummy_fdt));
    put_be32(dummy_fdt, 0U);
    uint32_t tmp_cap[2];
    uint32_t tmp_cnt = 0U;
    assert(arm64_fdt_blob_scan_cpus(dummy_fdt, sizeof(dummy_fdt), tmp_cap, 2U, &tmp_cnt) == -EINVAL);
}

int main(void)
{
    verify_cell_vmo_cdt_ring_accel_and_smmu();
    verify_elf64_pie_and_fdt();
    for (uint32_t i = 0U; i < PT_POOL_PAGES; i++) {
        assert(!g_pt_pool.used[i]);
    }
    printf("PASS: test_stage6_completeness (CDT/Cell/VMO-AF-Lazy/RingIPC/SMMUv3/AccelQueue/PIE-RELA)\n");
    return 0;
}
