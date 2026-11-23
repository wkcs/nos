/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Rockchip RK3588 RKNPU v2 (3-Core 6 TOPS) EL1 加速器驱动与 IOMMU/TaskDesc 编码
 */

#include <nos/accel.h>
#include <nos/hal_arch.h>
#include <nos/vmo.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/spinlock.h>
#include <string.h>

#ifndef RK3588_RKNPU_CORE0_BASE
#define RK3588_RKNPU_CORE0_BASE 0xFDAB0000UL
#endif

#ifndef RKNPU_IOMMU_BASE
#define RKNPU_IOMMU_BASE        0xFDAB9000UL
#endif

#define RKNPU_MAX_BOUND_VMOS    16U
#define RKNPU_BOUND_MAX_PAGES   16U
#define RKNPU_MAX_ASYNC_JOBS    16U
#define RKNPU_REGCFG_WORDS      16U

#ifndef RKNPU_TASK_DESC_DEFINED
#define RKNPU_TASK_DESC_DEFINED
struct rknpu_task_desc_wire {
    uint64_t regcfg_pa;
    uint32_t regcfg_amount;
    uint32_t flags;
    uint32_t int_mask;
    uint32_t int_clear;
    uint64_t input_iova;
    uint64_t weight_iova;
    uint64_t output_iova;
    uint32_t dim_m;
    uint32_t dim_n;
    uint32_t dim_k;
    uint32_t kernel_op;
};
#endif

struct rknpu_regcfg_cmd {
    uint32_t target;
    uint32_t val;
    uint32_t reg_offset;
    uint32_t flags;
};

struct rknpu_bound_vmo {
    bool            in_use;
    uint32_t        pasid;
    struct nos_vmo *vmo;
    uint64_t        iova_base;
    uint64_t        pte_first;
    uint32_t        page_count;
    uint32_t        size_bytes;
    uint64_t        page_pas[RKNPU_BOUND_MAX_PAGES];
};

struct rknpu_async_job {
    bool                        in_use;
    bool                        canceled;
    bool                        completed;
    uint32_t                    pasid;
    uint64_t                    req_id;
    struct nos_accel_job_desc   desc;
    struct rknpu_task_desc_wire task_desc;
    struct nos_accel_job_result result;
    int                         rc;
};

struct rknpu_driver_state {
    spinlock_t              lock;
    struct rknpu_bound_vmo  bound[RKNPU_MAX_BOUND_VMOS];
    struct rknpu_async_job  jobs[RKNPU_MAX_ASYNC_JOBS];
    struct rknpu_regcfg_cmd last_regcfg[RKNPU_REGCFG_WORDS];
    uint32_t                regcfg_seq;
};

static struct nos_accel_device g_rknpu_dev;
static struct rknpu_driver_state g_rknpu_state;

static inline uint64_t rknpu_iommu_pte_make(uint64_t paddr, bool writable)
{
    uint64_t pte = (paddr & ~0xFFFULL) | 0x1ULL | (1ULL << 2);
    if (writable) {
        pte |= (1ULL << 3);
    }
    return pte;
}

static void rknpu_encode_regcfg_seq(const struct nos_accel_job_desc *job,
                                    uint64_t in_iova,
                                    uint64_t w_iova,
                                    uint64_t out_iova,
                                    struct rknpu_regcfg_cmd *out_cmds)
{
    uint32_t i = 0U;

    if (out_cmds == NULL || job == NULL) {
        return;
    }

    memset(out_cmds, 0, sizeof(struct rknpu_regcfg_cmd) * RKNPU_REGCFG_WORDS);
    /* CNA (0x1000) / CORE (0x3000) / DPU (0x4000) / PC (0x0000) 寄存器序列编码 */
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0100U, 0x0001U, 0x0000U, 0U }; /* S_RST */
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0200U, (uint32_t)(in_iova & 0xFFFFFFFFULL), 0x1040U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0200U, (uint32_t)(in_iova >> 32), 0x1044U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0200U, (uint32_t)(w_iova & 0xFFFFFFFFULL), 0x1050U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0200U, (uint32_t)(w_iova >> 32), 0x1054U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0400U, (uint32_t)(out_iova & 0xFFFFFFFFULL), 0x4030U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0400U, (uint32_t)(out_iova >> 32), 0x4034U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0800U, job->dim_m, 0x3010U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0800U, job->dim_n, 0x3014U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0800U, job->dim_k, 0x3018U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0800U, job->kernel_op, 0x3020U, 0U };
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0100U, 0x03FFU, 0x0024U, 0U }; /* INT_MASK */
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0100U, 0x03FFU, 0x0028U, 0U }; /* INT_CLEAR */
    out_cmds[i++] = (struct rknpu_regcfg_cmd){ 0x0100U, 0x0001U, 0x0010U, 1U }; /* OP_EN */
}

static uint64_t rknpu_lookup_bound_iova_locked(const struct nos_vmo *vmo, uint32_t *out_pasid)
{
    if (vmo == NULL) {
        return 0ULL;
    }
    for (uint32_t i = 0U; i < RKNPU_MAX_BOUND_VMOS; i++) {
        if (g_rknpu_state.bound[i].in_use && g_rknpu_state.bound[i].vmo == vmo) {
            if (out_pasid != NULL && *out_pasid == 0U) {
                *out_pasid = g_rknpu_state.bound[i].pasid;
            }
            return g_rknpu_state.bound[i].iova_base;
        }
    }
    if (vmo->page_count > 0U && vmo->pages != NULL && vmo->pages[0] != NULL) {
        return (uint64_t)vmo->pages[0]->paddr;
    }
    return 0ULL;
}

int rknpu_build_task_desc(const struct nos_accel_job_desc *job,
                          uint64_t in_iova,
                          uint64_t w_iova,
                          uint64_t out_iova,
                          void *out_desc,
                          size_t out_desc_size)
{
    struct rknpu_task_desc_wire wire;

    if (job == NULL || out_desc == NULL || out_desc_size < sizeof(wire)) {
        return -EINVAL;
    }

    memset(&wire, 0, sizeof(wire));
    wire.regcfg_pa = (uint64_t)RK3588_RKNPU_CORE0_BASE;
    wire.regcfg_amount = RKNPU_REGCFG_WORDS;
    wire.flags = job->flags | 0x1U; /* PC_TASK_ENABLE */
    wire.int_mask = 0x3FFU;
    wire.int_clear = 0x3FFU;
    wire.input_iova = in_iova;
    wire.weight_iova = w_iova;
    wire.output_iova = out_iova;
    wire.dim_m = job->dim_m;
    wire.dim_n = job->dim_n;
    wire.dim_k = job->dim_k;
    wire.kernel_op = job->kernel_op;

    memcpy(out_desc, &wire, sizeof(wire));
    return 0;
}

static int rknpu_query_caps(struct nos_accel_device *dev, struct nos_accel_caps *out_caps)
{
    *out_caps = dev->caps;
    return 0;
}

static int rknpu_bind_vmo(struct nos_accel_device *dev, struct nos_vmo *vmo, uint32_t pasid)
{
    struct rknpu_bound_vmo *slot = NULL;
    uint32_t cache_pages;

    (void)dev;
    if (vmo == NULL || vmo->page_count == 0U || vmo->pages == NULL || vmo->pages[0] == NULL) {
        return -EINVAL;
    }

    cache_pages = vmo->page_count;
    if (cache_pages > RKNPU_BOUND_MAX_PAGES) {
        cache_pages = RKNPU_BOUND_MAX_PAGES;
    }

    spin_lock_irq(&g_rknpu_state.lock);
    for (uint32_t i = 0U; i < RKNPU_MAX_BOUND_VMOS; i++) {
        if (g_rknpu_state.bound[i].in_use &&
            g_rknpu_state.bound[i].vmo == vmo &&
            g_rknpu_state.bound[i].pasid == pasid) {
            slot = &g_rknpu_state.bound[i];
            break;
        }
        if (!g_rknpu_state.bound[i].in_use && slot == NULL) {
            slot = &g_rknpu_state.bound[i];
        }
    }
    if (slot == NULL) {
        spin_unlock_irq(&g_rknpu_state.lock);
        return -EBUSY;
    }

    if (!slot->in_use) {
        (void)nos_object_get(&vmo->header);
        slot->in_use = true;
    }
    slot->pasid = pasid;
    slot->vmo = vmo;
    slot->page_count = vmo->page_count;
    slot->size_bytes = vmo->size_bytes;
    slot->iova_base = (uint64_t)vmo->pages[0]->paddr;
    slot->pte_first = rknpu_iommu_pte_make(slot->iova_base, !vmo->sealed);
    memset(slot->page_pas, 0, sizeof(slot->page_pas));
    for (uint32_t p = 0U; p < cache_pages; p++) {
        slot->page_pas[p] = (vmo->pages[p] != NULL) ? (uint64_t)vmo->pages[p]->paddr : 0ULL;
    }
    spin_unlock_irq(&g_rknpu_state.lock);
    return 0;
}

static int rknpu_submit_job(struct nos_accel_device *dev,
                            struct task_struct *caller,
                            const struct nos_accel_job_desc *desc,
                            struct nos_vmo *weight_vmo,
                            struct nos_vmo *input_vmo,
                            struct nos_vmo *output_vmo,
                            struct nos_accel_job_result *out_res)
{
    struct rknpu_task_desc_wire task_desc;
    struct rknpu_regcfg_cmd regcfg[RKNPU_REGCFG_WORDS];
    uint64_t in_iova;
    uint64_t w_iova;
    uint64_t out_iova;
    uint32_t pasid = 0U;
    int rc;
    int exec_rc;

    (void)dev;
    (void)caller;
    if (desc == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&g_rknpu_state.lock);
    in_iova = rknpu_lookup_bound_iova_locked(input_vmo, &pasid);
    w_iova = rknpu_lookup_bound_iova_locked(weight_vmo, &pasid);
    out_iova = rknpu_lookup_bound_iova_locked(output_vmo, &pasid);
    spin_unlock_irq(&g_rknpu_state.lock);

    rc = rknpu_build_task_desc(desc, in_iova, w_iova, out_iova, &task_desc, sizeof(task_desc));
    if (rc != 0) {
        return rc;
    }

    rknpu_encode_regcfg_seq(desc, in_iova, w_iova, out_iova, regcfg);
    spin_lock_irq(&g_rknpu_state.lock);
    memcpy(g_rknpu_state.last_regcfg, regcfg, sizeof(regcfg));
    g_rknpu_state.regcfg_seq++;
    spin_unlock_irq(&g_rknpu_state.lock);

    exec_rc = (desc->kernel_op == NOS_ACCEL_KERNEL_NOP) ? 0 : -ENODEV;

    if ((desc->flags & NOS_ACCEL_FLAG_ASYNC) != 0U) {
        struct rknpu_async_job *job = NULL;

        if (exec_rc != 0) {
            return exec_rc;
        }
        spin_lock_irq(&g_rknpu_state.lock);
        for (uint32_t i = 0U; i < RKNPU_MAX_ASYNC_JOBS; i++) {
            if (g_rknpu_state.jobs[i].in_use && g_rknpu_state.jobs[i].req_id == desc->req_id) {
                spin_unlock_irq(&g_rknpu_state.lock);
                return -EEXIST;
            }
            if (!g_rknpu_state.jobs[i].in_use && job == NULL) {
                job = &g_rknpu_state.jobs[i];
            }
        }
        if (job == NULL) {
            spin_unlock_irq(&g_rknpu_state.lock);
            return -EBUSY;
        }
        memset(job, 0, sizeof(*job));
        job->in_use = true;
        job->pasid = pasid;
        job->req_id = desc->req_id;
        job->desc = *desc;
        job->task_desc = task_desc;
        job->rc = 0;
        job->result.req_id = desc->req_id;
        job->result.status = -EINPROGRESS;
        spin_unlock_irq(&g_rknpu_state.lock);

        if (out_res != NULL) {
            memset(out_res, 0, sizeof(*out_res));
            out_res->req_id = desc->req_id;
            out_res->status = -EINPROGRESS;
        }
        return 0;
    }

    if (out_res != NULL) {
        out_res->req_id = desc->req_id;
        out_res->status = exec_rc;
        out_res->output_bytes = 0U;
    }
    return exec_rc;
}

static int rknpu_wait_job(struct nos_accel_device *dev,
                          uint64_t req_id,
                          struct nos_accel_job_result *out_res,
                          uint32_t timeout_ms)
{
    struct nos_accel_job_result res;
    int rc = -ENOENT;

    (void)dev;
    (void)timeout_ms;
    memset(&res, 0, sizeof(res));
    res.req_id = req_id;

    spin_lock_irq(&g_rknpu_state.lock);
    for (uint32_t i = 0U; i < RKNPU_MAX_ASYNC_JOBS; i++) {
        struct rknpu_async_job *job = &g_rknpu_state.jobs[i];
        if (job->in_use && job->req_id == req_id) {
            rc = job->canceled ? -ECANCELED : job->rc;
            res = job->result;
            res.status = rc;
            memset(job, 0, sizeof(*job));
            break;
        }
    }
    spin_unlock_irq(&g_rknpu_state.lock);

    if (out_res != NULL && rc != -ENOENT) {
        *out_res = res;
    }
    return rc;
}

static int rknpu_cancel_job(struct nos_accel_device *dev, uint64_t req_id)
{
    int rc = -ENOENT;

    (void)dev;
    spin_lock_irq(&g_rknpu_state.lock);
    for (uint32_t i = 0U; i < RKNPU_MAX_ASYNC_JOBS; i++) {
        struct rknpu_async_job *job = &g_rknpu_state.jobs[i];
        if (job->in_use && job->req_id == req_id) {
            memset(job, 0, sizeof(*job));
            rc = 0;
            break;
        }
    }
    spin_unlock_irq(&g_rknpu_state.lock);
    return rc;
}

static int rknpu_reset_ctx(struct nos_accel_device *dev, uint32_t pasid)
{
    struct nos_vmo *drop_vmos[RKNPU_MAX_BOUND_VMOS];
    uint32_t drop_cnt = 0U;

    (void)dev;
    spin_lock_irq(&g_rknpu_state.lock);
    for (uint32_t i = 0U; i < RKNPU_MAX_ASYNC_JOBS; i++) {
        struct rknpu_async_job *job = &g_rknpu_state.jobs[i];
        if (job->in_use && (pasid == 0U || job->pasid == pasid)) {
            memset(job, 0, sizeof(*job));
        }
    }
    for (uint32_t i = 0U; i < RKNPU_MAX_BOUND_VMOS; i++) {
        struct rknpu_bound_vmo *bv = &g_rknpu_state.bound[i];
        if (bv->in_use && (pasid == 0U || bv->pasid == pasid)) {
            if (bv->vmo != NULL) {
                drop_vmos[drop_cnt++] = bv->vmo;
            }
            memset(bv, 0, sizeof(*bv));
        }
    }
    spin_unlock_irq(&g_rknpu_state.lock);

    for (uint32_t i = 0U; i < drop_cnt; i++) {
        (void)nos_object_put(&drop_vmos[i]->header);
    }
    return 0;
}

static const struct nos_accel_ops g_rknpu_ops = {
    .query_caps = rknpu_query_caps,
    .bind_vmo   = rknpu_bind_vmo,
    .submit_job = rknpu_submit_job,
    .cancel_job = rknpu_cancel_job,
    .wait_job   = rknpu_wait_job,
    .reset_ctx  = rknpu_reset_ctx,
};

int rknpu_accel_init(void)
{
    struct nos_accel_caps caps;

    memset(&g_rknpu_state, 0, sizeof(g_rknpu_state));
    spin_lock_init(&g_rknpu_state.lock);

    memset(&caps, 0, sizeof(caps));
    caps.accel_type = NOS_ACCEL_TYPE_RKNPU;
    caps.core_count = 3U;
    caps.stream_id = 2U;
    caps.flags = NOS_ACCEL_CAP_SVA | NOS_ACCEL_CAP_ASYNC_QUEUE | NOS_ACCEL_CAP_INT8_MATMUL;
    caps.supported_kernels = (1ULL << NOS_ACCEL_KERNEL_NOP) |
                             (1ULL << NOS_ACCEL_KERNEL_MATMUL_I8) |
                             (1ULL << NOS_ACCEL_KERNEL_GEMV_I32);
    caps.peak_tops_x100 = 600ULL;
    caps.max_tensor_bytes = 256ULL * 1024ULL * 1024ULL;
    memcpy(caps.name, "rknpu-v2-3core", 15U);

    return nos_accel_register(&g_rknpu_dev, &caps, &g_rknpu_ops, NULL);
}

#if defined(CONFIG_SOC_RK3588) && !defined(__HOST_TEST__)
#include <kernel/init.h>
SYS_INIT(rknpu_accel_init, INIT_LEVEL_POST_KERNEL);
#endif
