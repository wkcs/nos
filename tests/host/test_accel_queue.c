/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：AccelQueue、VirtIO-Accel 纯整数张量算子、Sealed 输出拦截、
 * SMMUv3 SVA 绑定、RingIPC 直通单次扣费与 RKNPU 硬件描述编码
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
#include <nos/iommu.h>
#include <nos/ring.h>
#include <nos/vmo.h>
#include <asm/mmu.h>
#include <asm/smmuv3.h>
#include <soc/rk3588.h>

int virtio_accel_init(void);
int rknpu_accel_init(void);
int rknpu_build_task_desc(const struct nos_accel_job_desc *job,
                          uint64_t in_iova,
                          uint64_t w_iova,
                          uint64_t out_iova,
                          void *out_desc,
                          size_t out_desc_size);

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

static uint64_t g_mono_ns = 20000000ULL;

uint64_t hal_timer_monotonic_ns(void)
{
    return __atomic_add_fetch(&g_mono_ns, 1000ULL, __ATOMIC_RELAXED);
}

/* 用于 SMMUv3 + Cell 页表联调的 4KB 页表池 */
#define PT_POOL_PAGES 128U
#define PT_POOL_PBASE 0x80000000ULL

struct host_pt_pool {
    uint8_t mem[PT_POOL_PAGES][4096];
    bool    used[PT_POOL_PAGES];
};

static struct host_pt_pool g_pt_pool;
static uint16_t g_next_asid = 1U;

static uintptr_t pt_alloc(void *ctx)
{
    struct host_pt_pool *p = (struct host_pt_pool *)ctx;
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
    struct host_pt_pool *p = (struct host_pt_pool *)ctx;
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
    struct host_pt_pool *p = (struct host_pt_pool *)ctx;
    assert(pa >= PT_POOL_PBASE);
    uint64_t off = pa - PT_POOL_PBASE;
    assert(off + 4096ULL <= sizeof(p->mem));
    return (void *)((uint8_t *)p->mem + off);
}

static const struct arm64_pt_backend g_pt_be = {
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
        arm64_pt_destroy(space->root_pt_phys, 39, &g_pt_be);
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
                              prot_flags, true, &g_pt_be);
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_unmap_range(space->root_pt_phys, 39, vaddr, size, false, &g_pt_be);
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
    if (notif == NULL || bits == 0ULL) {
        return -EINVAL;
    }
    if (nos_object_is_closed(&notif->object)) {
        return -EPIPE;
    }
    __atomic_or_fetch(&notif->pending_bits, bits, __ATOMIC_RELEASE);
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

static void test_accel_gemv_matmul_sealed_sva_and_ring(void)
{
    struct nos_cell_budget budget = {
        .cpu_ns_limit = 0ULL,
        .mem_bytes_limit = 262144ULL,
        .tool_calls_limit = 4ULL,
        .accel_ops_limit = 4ULL,
        .max_child_cells = 0U,
    };
    struct nos_vmo_create_args vargs = {
        .size_bytes = 4096U,
        .kind = NOS_VMO_DMA_TENSOR,
        .flags = 0U,
    };
    struct nos_cell *cell;
    struct nos_accel_device *dev;
    struct nos_accel_caps caps;
    struct task_struct client;
    struct nos_vmo *w_vmo;
    struct nos_vmo *in_vmo;
    struct nos_vmo *out_vmo;
    nos_handle_t accel_h;
    nos_handle_t w_h;
    nos_handle_t in_h;
    nos_handle_t out_h;
    nos_handle_t notif_h;
    struct nos_notification job_notif;
    int32_t w_gemv[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int32_t x_gemv[4] = { 2, 1, 0, 3 };
    int32_t y_gemv[2] = { 0, 0 };
    int8_t a_mat[4] = { 1, 2, 3, 4 };
    int8_t b_mat[4] = { 5, 6, 7, 8 };
    int32_t c_mat[4] = { 0 };
    int32_t relu_in[4] = { -5, 7, -1, 9 };
    int32_t relu_out[4] = { 0 };
    struct nos_accel_job_desc desc;
    struct nos_accel_job_result res;
    struct nos_iommu_info iinfo;
    uintptr_t xlate_pa = 0U;
    uint32_t xlate_prot = 0U;
    uint32_t pasid = 0U;

    smmuv3_host_reset();
    smmuv3_host_set_pt_backend(&g_pt_be);
    assert(arm64_smmuv3_init(0UL, 0UL) == 0); /* 影子隔离模式 */

    nos_accel_host_reset();
    assert(virtio_accel_init() == 0);
    dev = nos_accel_get_default();
    assert(dev != NULL);
    assert(nos_accel_query_caps(dev, &caps) == 0);
    assert(caps.accel_type == NOS_ACCEL_TYPE_VIRTIO);
    assert(caps.stream_id == 1U);
    assert(strcmp(caps.name, "virtio-accel-el1") == 0);

    cell = nos_cell_create(NULL, &budget);
    assert(cell != NULL);

    memset(&client, 0, sizeof(client));
    nos_refcount_init(&client.refcount, 1U);
    client.cell = cell;

    w_vmo = nos_vmo_create(&vargs);
    in_vmo = nos_vmo_create(&vargs);
    out_vmo = nos_vmo_create(&vargs);
    assert(w_vmo != NULL && in_vmo != NULL && out_vmo != NULL);

    assert(nos_vmo_write(w_vmo, 0U, w_gemv, sizeof(w_gemv)) == 0);
    assert(nos_vmo_seal(w_vmo) == 0);
    assert(nos_vmo_write(in_vmo, 0U, x_gemv, sizeof(x_gemv)) == 0);

    /* 将输出与只读权重 VMO 映射到 Cell 用户态地址空间，以供 SMMUv3 SVA 校验 */
    assert(nos_vmo_map(out_vmo, nos_cell_vm_space(cell), 0x470000UL, 0U, 4096U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER, NULL) == 0);
    assert(nos_vmo_map(w_vmo, nos_cell_vm_space(cell), 0x471000UL, 0U, 4096U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_USER, NULL) == 0);

    assert(nos_notification_init(&job_notif) == 0);

    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_ACCEL, dev,
                         NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_SEND, 0U, &accel_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, w_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &w_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, in_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &in_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, out_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &out_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_NOTIFICATION, &job_notif,
                         NOS_CAP_RIGHT_SEND, 0U, &notif_h) == 0);

    /* 1. 验证 GEMV_I32 精确输出: [1*2+2*1+3*0+4*3, 5*2+6*1+7*0+8*3] = [16, 40] */
    memset(&desc, 0, sizeof(desc));
    desc.kernel_op = NOS_ACCEL_KERNEL_GEMV_I32;
    desc.req_id = 1001ULL;
    desc.weight_vmo_cap = w_h;
    desc.input_vmo_cap = in_h;
    desc.output_vmo_cap = out_h;
    desc.notify_cap = notif_h;
    desc.dim_m = 2U;
    desc.dim_n = 1U;
    desc.dim_k = 4U;

    memset(&res, 0, sizeof(res));
    assert(nos_accel_submit(dev, &client, &desc, &res) == 0);
    assert(res.req_id == 1001ULL && res.status == 0 && res.output_bytes == 8U);
    assert(nos_vmo_read(out_vmo, 0U, y_gemv, sizeof(y_gemv)) == 0);
    assert(y_gemv[0] == 16 && y_gemv[1] == 40);

    uint64_t nbits = 0ULL;
    assert(nos_notify_wait_obj(&job_notif, &nbits, NOS_NO_WAIT) == 0);
    assert(nbits == 1ULL);

    /* 2. 验证 SMMUv3 SVA 已绑定该 Cell 空间且读写隔离有效 */
    assert(nos_iommu_get_status(&iinfo) == 0);
    if (iinfo.max_pasids > 1U) {
        pasid = cell->cell_id % iinfo.max_pasids;
    }
    assert(nos_iommu_translate(caps.stream_id, pasid, 0x470000UL, &xlate_pa, &xlate_prot) == 0);
    assert((xlate_prot & HAL_VM_PROT_WRITE) != 0U);
    assert(nos_iommu_translate(caps.stream_id, pasid, 0x471000UL, &xlate_pa, &xlate_prot) == 0);
    assert((xlate_prot & HAL_VM_PROT_WRITE) == 0U);

    /* 3. 验证 Sealed 输出 VMO 拦截 (-EACCES)，且不额外消耗配额 */
    desc.output_vmo_cap = w_h;
    assert(nos_accel_submit(dev, &client, &desc, &res) == -EACCES);

    /* 4. 验证 MATMUL_I8: [[1,2],[3,4]] * [[5,6],[7,8]] = [[19,22],[43,50]] */
    struct nos_vmo *w2_vmo = nos_vmo_create(&vargs);
    struct nos_vmo *in2_vmo = nos_vmo_create(&vargs);
    nos_handle_t w2_h;
    nos_handle_t in2_h;
    assert(w2_vmo != NULL && in2_vmo != NULL);
    assert(nos_vmo_write(w2_vmo, 0U, b_mat, sizeof(b_mat)) == 0);
    assert(nos_vmo_write(in2_vmo, 0U, a_mat, sizeof(a_mat)) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, w2_vmo,
                         NOS_CAP_RIGHT_READ, 0U, &w2_h) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, in2_vmo,
                         NOS_CAP_RIGHT_READ, 0U, &in2_h) == 0);

    memset(&desc, 0, sizeof(desc));
    desc.kernel_op = NOS_ACCEL_KERNEL_MATMUL_I8;
    desc.req_id = 1002ULL;
    desc.weight_vmo_cap = w2_h;
    desc.input_vmo_cap = in2_h;
    desc.output_vmo_cap = out_h;
    desc.dim_m = 2U;
    desc.dim_n = 2U;
    desc.dim_k = 2U;
    assert(nos_accel_submit(dev, &client, &desc, &res) == 0);
    assert(res.output_bytes == 16U);
    assert(nos_vmo_read(out_vmo, 0U, c_mat, sizeof(c_mat)) == 0);
    assert(c_mat[0] == 19 && c_mat[1] == 22 && c_mat[2] == 43 && c_mat[3] == 50);

    /* 5. 通过 RingIPC 提交 NOS_RING_OP_ACCEL_JOB (RELU_I32)，验证无双扣费 */
    uint64_t schema = (uint64_t)NOS_ACCEL_KERNEL_RELU_I32;
    struct nos_ring_create_args rargs = {
        .sq_entries = 8U,
        .cq_entries = 8U,
        .flags = NOS_RING_CREATE_SCHEMA_FILTER,
        .schema_count = 1U,
        .schema_list_ptr = (uint64_t)(uintptr_t)&schema,
    };
    struct nos_ring *ring = nos_ring_create(&rargs, &schema, nos_cell_vm_space(cell), cell);
    assert(ring != NULL);

    assert(nos_vmo_write(in2_vmo, 0U, relu_in, sizeof(relu_in)) == 0);
    struct nos_ring_header *hdr = (struct nos_ring_header *)ring->ring_vmo->pages[0]->kvaddr;
    struct nos_ring_sqe *sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    struct nos_ring_cqe *cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);

    memset(&sq[0], 0, sizeof(sq[0]));
    sq[0].opcode = NOS_RING_OP_ACCEL_JOB;
    sq[0].target_cap = accel_h;
    sq[0].req_id = 2001ULL;
    sq[0].schema_id = (uint64_t)NOS_ACCEL_KERNEL_RELU_I32;
    sq[0].vmo_cap = in2_h;
    sq[0].vmo_offset = 0ULL;
    sq[0].vmo_len = sizeof(relu_in);
    sq[0].inline_arg[0] = ((uint64_t)out_h << 32) | (uint64_t)NOS_HANDLE_INVALID;
    sq[0].inline_arg[1] = 4ULL; /* dim_m = 4 */

    __atomic_store_n(&hdr->sq_tail, 1U, __ATOMIC_RELEASE);
    assert(nos_ring_enter(ring, &client, 1U, 1U,
                          NOS_RING_ENTER_SUBMIT | NOS_RING_ENTER_WAIT_CQ,
                          NOS_IPC_TIMEOUT_INFINITE) == 1);
    assert(cq[0].req_id == 2001ULL && cq[0].status == 0 && cq[0].result_len == 16U);
    assert(nos_vmo_read(out_vmo, 0U, relu_out, sizeof(relu_out)) == 0);
    assert(relu_out[0] == 0 && relu_out[1] == 7 && relu_out[2] == 0 && relu_out[3] == 9);

    /* 已用次数应严格为 3 (GEMV + MATMUL + RELU_RING)，第 4 次成功，第 5 次命中 -EDQUOT */
    const struct nos_cell_telemetry *tm = nos_cell_telemetry_view(cell);
    assert(tm->accel_ops_used == 3ULL);

    memset(&desc, 0, sizeof(desc));
    desc.kernel_op = NOS_ACCEL_KERNEL_NOP;
    desc.req_id = 3001ULL;
    desc.weight_vmo_cap = NOS_HANDLE_INVALID;
    desc.input_vmo_cap = NOS_HANDLE_INVALID;
    desc.output_vmo_cap = NOS_HANDLE_INVALID;
    desc.notify_cap = NOS_HANDLE_INVALID;
    assert(nos_accel_submit(dev, &client, &desc, &res) == 0);
    assert(tm->accel_ops_used == 4ULL);

    desc.req_id = 3002ULL;
    assert(nos_accel_submit(dev, &client, &desc, &res) == -EDQUOT);

    struct nos_audit_record audits[32];
    int an = nos_cell_read_audit(cell, audits, 32U);
    bool seen_submit = false;
    bool seen_quota = false;
    for (int i = 0; i < an; i++) {
        if (audits[i].event_type == NOS_AUDIT_ACCEL_SUBMIT) {
            seen_submit = true;
        } else if (audits[i].event_type == NOS_AUDIT_BUDGET_HIT) {
            seen_quota = true;
        }
    }
    assert(seen_submit && seen_quota);

    assert(nos_ring_destroy(ring) == 0);
    (void)nos_object_close(&job_notif.object);
    (void)nos_object_retire(&job_notif.object);
    (void)nos_object_close(&w2_vmo->header);
    (void)nos_object_retire(&w2_vmo->header);
    (void)nos_object_close(&in2_vmo->header);
    (void)nos_object_retire(&in2_vmo->header);
    (void)nos_object_close(&out_vmo->header);
    (void)nos_object_retire(&out_vmo->header);
    (void)nos_object_close(&in_vmo->header);
    (void)nos_object_retire(&in_vmo->header);
    (void)nos_object_close(&w_vmo->header);
    (void)nos_object_retire(&w_vmo->header);
    (void)nos_object_put(&dev->header);
    assert(nos_cell_destroy(cell) == 0);
    nos_accel_host_reset();
    smmuv3_host_reset();
}

static void test_rknpu_desc_and_iommu_pte(void)
{
    struct rknpu_hw_task_desc hw_desc;
    struct nos_accel_job_desc job;
    struct nos_accel_device *rknpu_dev;
    struct nos_accel_caps rcaps;
    uint32_t ro_pte;
    uint32_t rw_pte;

    nos_accel_host_reset();
    assert(rknpu_accel_init() == 0);
    rknpu_dev = nos_accel_find_by_type(NOS_ACCEL_TYPE_RKNPU);
    assert(rknpu_dev != NULL);
    assert(nos_accel_query_caps(rknpu_dev, &rcaps) == 0);
    assert(rcaps.core_count == 3U && rcaps.peak_tops_x100 == 600ULL);
    (void)nos_object_put(&rknpu_dev->header);

    memset(&job, 0, sizeof(job));
    job.kernel_op = NOS_ACCEL_KERNEL_MATMUL_I8;
    job.dim_m = 16U;
    job.dim_n = 32U;
    job.dim_k = 64U;

    assert(rknpu_build_task_desc(&job, 0x100000ULL, 0x200000ULL, 0x300000ULL,
                                 &hw_desc, sizeof(hw_desc)) == 0);
    assert(hw_desc.regcfg_pa == (uint64_t)RK3588_RKNPU_CORE0_BASE);
    assert((hw_desc.flags & 0x1U) != 0U);
    assert(hw_desc.input_iova == 0x100000ULL);
    assert(hw_desc.weight_iova == 0x200000ULL);
    assert(hw_desc.output_iova == 0x300000ULL);
    assert(hw_desc.dim_m == 16U && hw_desc.dim_n == 32U && hw_desc.dim_k == 64U);

    ro_pte = rknpu_iommu_pte_make(0x90001000ULL, 0);
    rw_pte = rknpu_iommu_pte_make(0x90002000ULL, 1);
    assert((ro_pte & 0xFFFFF000U) == 0x90001000U && (ro_pte & 0x1U) != 0U);
    assert((ro_pte & (1U << 2)) != 0U && (ro_pte & (1U << 1)) == 0U);
    assert((rw_pte & (1U << 1)) != 0U);

    nos_accel_host_reset();
}

int main(void)
{
    test_accel_gemv_matmul_sealed_sva_and_ring();
    test_rknpu_desc_and_iommu_pte();
    printf("PASS: test_accel_queue (GEMV_I32/MATMUL_I8/RELU, Sealed RO, SMMUv3 SVA, RingIPC single-charge, -EDQUOT, RKNPU)\n");
    return 0;
}
