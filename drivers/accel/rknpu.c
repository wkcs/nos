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
#include <string.h>

#ifndef RK3588_RKNPU_CORE0_BASE
#define RK3588_RKNPU_CORE0_BASE 0xFDAB0000UL
#endif

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

static struct nos_accel_device g_rknpu_dev;

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
    wire.regcfg_amount = 16U;
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

static int rknpu_submit_job(struct nos_accel_device *dev,
                            struct task_struct *caller,
                            const struct nos_accel_job_desc *desc,
                            struct nos_vmo *weight_vmo,
                            struct nos_vmo *input_vmo,
                            struct nos_vmo *output_vmo,
                            struct nos_accel_job_result *out_res)
{
    struct rknpu_task_desc_wire task_desc;
    uint64_t in_pa = (input_vmo != NULL && input_vmo->page_count > 0U && input_vmo->pages[0] != NULL)
                     ? (uint64_t)input_vmo->pages[0]->paddr : 0ULL;
    uint64_t w_pa = (weight_vmo != NULL && weight_vmo->page_count > 0U && weight_vmo->pages[0] != NULL)
                    ? (uint64_t)weight_vmo->pages[0]->paddr : 0ULL;
    uint64_t out_pa = (output_vmo != NULL && output_vmo->page_count > 0U && output_vmo->pages[0] != NULL)
                      ? (uint64_t)output_vmo->pages[0]->paddr : 0ULL;
    int rc;

    (void)dev;
    (void)caller;
    rc = rknpu_build_task_desc(desc, in_pa, w_pa, out_pa, &task_desc, sizeof(task_desc));
    if (rc != 0) {
        return rc;
    }

    if (out_res != NULL) {
        out_res->req_id = desc->req_id;
        out_res->status = (desc->kernel_op == NOS_ACCEL_KERNEL_NOP) ? 0 : -ENODEV;
        out_res->output_bytes = 0U;
    }
    return (desc->kernel_op == NOS_ACCEL_KERNEL_NOP) ? 0 : -ENODEV;
}

static const struct nos_accel_ops g_rknpu_ops = {
    .query_caps = rknpu_query_caps,
    .bind_vmo   = NULL,
    .submit_job = rknpu_submit_job,
    .cancel_job = NULL,
    .reset_ctx  = NULL,
};

int rknpu_accel_init(void)
{
    struct nos_accel_caps caps;

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
