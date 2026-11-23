/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL1 混合内核加速器框架 (AccelQueue / NOS_CAP_ACCEL)
 * 负责 VMO 能力/Sealed 门禁、Agent Cell 5D 硬预算扣费、SMMUv3 SVA 绑定、审计与作业分发。
 */

#define pr_fmt(fmt) "[ACCEL]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/accel.h>
#include <nos/cap.h>
#include <nos/cell.h>
#include <nos/iommu.h>
#include <nos/notification.h>
#include <nos/vmo.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <string.h>

#ifdef __HOST_TEST__
#include <stdlib.h>
#endif

#define NOS_ACCEL_MAX_DEVICES   8U

static struct nos_accel_device *g_accel_devs[NOS_ACCEL_MAX_DEVICES];
static spinlock_t g_accel_list_lock;
static bool g_accel_lock_inited;

/* 弱符号默认值：允许在未链接硬件 SMMUv3 的宿主单测/小架构中平滑工作 */
__attribute__((weak)) int nos_iommu_get_status(struct nos_iommu_info *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->max_pasids = 1U;
    return 0;
}

__attribute__((weak)) int nos_iommu_bind_space(uint32_t stream_id,
                                               uint32_t pasid,
                                               struct nos_vm_space *space)
{
    (void)stream_id;
    (void)pasid;
    (void)space;
    return 0;
}

static void accel_ensure_lock(void)
{
    if (!g_accel_lock_inited) {
        spin_lock_init(&g_accel_list_lock);
        g_accel_lock_inited = true;
    }
}

static void accel_device_release(struct nos_object *obj)
{
    (void)obj;
    /* 驱动静态实例仅重置注册标志，由具体驱动负责生命周期 */
}

int nos_accel_register(struct nos_accel_device *dev,
                       const struct nos_accel_caps *caps,
                       const struct nos_accel_ops *ops,
                       void *priv)
{
    uint32_t i;

    if (dev == NULL || caps == NULL || ops == NULL || ops->submit_job == NULL) {
        return -EINVAL;
    }

    accel_ensure_lock();
    spin_lock_irq(&g_accel_list_lock);
    if (dev->registered) {
        spin_unlock_irq(&g_accel_list_lock);
        return -EALREADY;
    }

    for (i = 0U; i < NOS_ACCEL_MAX_DEVICES; i++) {
        if (g_accel_devs[i] == NULL) {
            memset(dev, 0, sizeof(*dev));
            (void)nos_object_init(&dev->header, true);
            nos_object_set_release(&dev->header, accel_device_release);
            dev->caps = *caps;
            dev->ops = ops;
            dev->priv = priv;
            dev->registered = true;
            dev->index = i;
            g_accel_devs[i] = dev;
            spin_unlock_irq(&g_accel_list_lock);
            return 0;
        }
    }

    spin_unlock_irq(&g_accel_list_lock);
    return -ENOSPC;
}

void nos_accel_unregister(struct nos_accel_device *dev)
{
    if (dev == NULL) {
        return;
    }

    accel_ensure_lock();
    spin_lock_irq(&g_accel_list_lock);
    if (dev->registered && dev->index < NOS_ACCEL_MAX_DEVICES &&
        g_accel_devs[dev->index] == dev) {
        g_accel_devs[dev->index] = NULL;
        dev->registered = false;
    }
    spin_unlock_irq(&g_accel_list_lock);
    (void)nos_object_close(&dev->header);
}

struct nos_accel_device *nos_accel_find_by_type(uint32_t accel_type)
{
    uint32_t i;
    struct nos_accel_device *found = NULL;

    accel_ensure_lock();
    spin_lock_irq(&g_accel_list_lock);
    for (i = 0U; i < NOS_ACCEL_MAX_DEVICES; i++) {
        struct nos_accel_device *d = g_accel_devs[i];
        if (d != NULL && d->registered && d->caps.accel_type == accel_type) {
            if (nos_object_get(&d->header)) {
                found = d;
            }
            break;
        }
    }
    spin_unlock_irq(&g_accel_list_lock);
    return found;
}

struct nos_accel_device *nos_accel_get_default(void)
{
    struct nos_accel_device *d = nos_accel_find_by_type(NOS_ACCEL_TYPE_VIRTIO);
    if (d != NULL) {
        return d;
    }

    accel_ensure_lock();
    spin_lock_irq(&g_accel_list_lock);
    for (uint32_t i = 0U; i < NOS_ACCEL_MAX_DEVICES; i++) {
        if (g_accel_devs[i] != NULL && g_accel_devs[i]->registered &&
            nos_object_get(&g_accel_devs[i]->header)) {
            d = g_accel_devs[i];
            break;
        }
    }
    spin_unlock_irq(&g_accel_list_lock);
    return d;
}

int nos_accel_query_caps(struct nos_accel_device *dev, struct nos_accel_caps *out_caps)
{
    if (dev == NULL || out_caps == NULL || !dev->registered) {
        return -EINVAL;
    }
    if (dev->ops != NULL && dev->ops->query_caps != NULL) {
        return dev->ops->query_caps(dev, out_caps);
    }
    *out_caps = dev->caps;
    return 0;
}

int nos_accel_bind_vmo(struct nos_accel_device *dev,
                       struct task_struct *caller,
                       nos_handle_t vmo_cap)
{
    struct nos_cap_table *cap_tbl;
    struct nos_vmo *vmo = NULL;
    uint32_t pasid = 0U;
    int rc;

    if (dev == NULL || !dev->registered || caller == NULL) {
        return -EINVAL;
    }

    cap_tbl = nos_task_active_cap_table(caller);
    if (cap_tbl == NULL) {
        return -EBADF;
    }

    rc = nos_cap_lookup(cap_tbl, vmo_cap, NOS_CAP_VMO, NOS_CAP_RIGHT_READ,
                        (void **)&vmo, NULL);
    if (rc < 0) {
        return rc;
    }

    if (caller->cell != NULL) {
        struct nos_iommu_info iinfo;
        (void)nos_iommu_get_status(&iinfo);
        if (iinfo.max_pasids > 1U) {
            pasid = caller->cell->cell_id % iinfo.max_pasids;
        }
        (void)nos_iommu_bind_space(dev->caps.stream_id, pasid,
                                   nos_cell_vm_space(caller->cell));
    }

    if (dev->ops != NULL && dev->ops->bind_vmo != NULL) {
        rc = dev->ops->bind_vmo(dev, vmo, pasid);
    } else {
        rc = 0;
    }

    (void)nos_object_put(&vmo->header);
    return rc;
}

static bool accel_check_vmo_span(const struct nos_vmo *vmo, uint32_t off, uint64_t bytes)
{
    if (vmo == NULL || (uint64_t)off > (uint64_t)vmo->size_bytes) {
        return false;
    }
    return bytes <= ((uint64_t)vmo->size_bytes - (uint64_t)off);
}

static int accel_validate_tensor_bounds(const struct nos_accel_job_desc *desc,
                                        const struct nos_vmo *w_vmo,
                                        const struct nos_vmo *in_vmo,
                                        const struct nos_vmo *out_vmo)
{
    uint64_t w_need = 0ULL;
    uint64_t in_need = 0ULL;
    uint64_t out_need = 0ULL;

    switch (desc->kernel_op) {
    case NOS_ACCEL_KERNEL_NOP:
        return 0;

    case NOS_ACCEL_KERNEL_GEMV_I32:
        if (desc->dim_m == 0U || desc->dim_k == 0U ||
            desc->dim_m > 65536U || desc->dim_k > 65536U) {
            return -EINVAL;
        }
        w_need = (uint64_t)desc->dim_m * (uint64_t)desc->dim_k * 4ULL;
        in_need = (uint64_t)desc->dim_k * 4ULL;
        out_need = (uint64_t)desc->dim_m * 4ULL;
        if (w_vmo == NULL || in_vmo == NULL || out_vmo == NULL) {
            return -EINVAL;
        }
        if (!accel_check_vmo_span(w_vmo, desc->weight_offset, w_need) ||
            !accel_check_vmo_span(in_vmo, desc->input_offset, in_need) ||
            !accel_check_vmo_span(out_vmo, desc->output_offset, out_need)) {
            return -EOVERFLOW;
        }
        return 0;

    case NOS_ACCEL_KERNEL_MATMUL_I8:
        if (desc->dim_m == 0U || desc->dim_n == 0U || desc->dim_k == 0U ||
            desc->dim_m > 4096U || desc->dim_n > 4096U || desc->dim_k > 4096U) {
            return -EINVAL;
        }
        in_need = (uint64_t)desc->dim_m * (uint64_t)desc->dim_k;
        w_need = (uint64_t)desc->dim_k * (uint64_t)desc->dim_n;
        out_need = (uint64_t)desc->dim_m * (uint64_t)desc->dim_n * 4ULL;
        if (w_vmo == NULL || in_vmo == NULL || out_vmo == NULL) {
            return -EINVAL;
        }
        if (!accel_check_vmo_span(w_vmo, desc->weight_offset, w_need) ||
            !accel_check_vmo_span(in_vmo, desc->input_offset, in_need) ||
            !accel_check_vmo_span(out_vmo, desc->output_offset, out_need)) {
            return -EOVERFLOW;
        }
        return 0;

    case NOS_ACCEL_KERNEL_RELU_I32:
        if (desc->dim_m == 0U || desc->dim_m > 1048576U) {
            return -EINVAL;
        }
        in_need = (uint64_t)desc->dim_m * 4ULL;
        out_need = in_need;
        if (in_vmo == NULL || out_vmo == NULL) {
            return -EINVAL;
        }
        if (!accel_check_vmo_span(in_vmo, desc->input_offset, in_need) ||
            !accel_check_vmo_span(out_vmo, desc->output_offset, out_need)) {
            return -EOVERFLOW;
        }
        return 0;

    default:
        return -EINVAL;
    }
}

int nos_accel_exec_cpu_kernel(uint32_t kernel_op,
                              const void *w_buf,
                              const void *in_buf,
                              void *out_buf,
                              uint32_t dim_m,
                              uint32_t dim_n,
                              uint32_t dim_k,
                              uint32_t *out_bytes)
{
    if (out_bytes != NULL) {
        *out_bytes = 0U;
    }

    switch (kernel_op) {
    case NOS_ACCEL_KERNEL_NOP:
        return 0;

    case NOS_ACCEL_KERNEL_GEMV_I32: {
        const int32_t *w = (const int32_t *)w_buf;
        const int32_t *x = (const int32_t *)in_buf;
        int32_t *y = (int32_t *)out_buf;
        uint32_t m;
        uint32_t k;

        if (w == NULL || x == NULL || y == NULL || dim_m == 0U || dim_k == 0U) {
            return -EINVAL;
        }
        for (m = 0U; m < dim_m; m++) {
            int64_t acc = 0;
            const int32_t *row = &w[(size_t)m * (size_t)dim_k];
            for (k = 0U; k < dim_k; k++) {
                acc += (int64_t)row[k] * (int64_t)x[k];
            }
            y[m] = (int32_t)acc;
        }
        if (out_bytes != NULL) {
            *out_bytes = dim_m * 4U;
        }
        return 0;
    }

    case NOS_ACCEL_KERNEL_MATMUL_I8: {
        const int8_t *a = (const int8_t *)in_buf;
        const int8_t *b = (const int8_t *)w_buf;
        int32_t *c = (int32_t *)out_buf;
        uint32_t m;
        uint32_t n;
        uint32_t k;

        if (a == NULL || b == NULL || c == NULL ||
            dim_m == 0U || dim_n == 0U || dim_k == 0U) {
            return -EINVAL;
        }
        for (m = 0U; m < dim_m; m++) {
            for (n = 0U; n < dim_n; n++) {
                int32_t acc = 0;
                for (k = 0U; k < dim_k; k++) {
                    acc += (int32_t)a[(size_t)m * (size_t)dim_k + k] *
                           (int32_t)b[(size_t)k * (size_t)dim_n + n];
                }
                c[(size_t)m * (size_t)dim_n + n] = acc;
            }
        }
        if (out_bytes != NULL) {
            *out_bytes = dim_m * dim_n * 4U;
        }
        return 0;
    }

    case NOS_ACCEL_KERNEL_RELU_I32: {
        const int32_t *x = (const int32_t *)in_buf;
        int32_t *y = (int32_t *)out_buf;
        uint32_t i;

        if (x == NULL || y == NULL || dim_m == 0U) {
            return -EINVAL;
        }
        for (i = 0U; i < dim_m; i++) {
            y[i] = (x[i] > 0) ? x[i] : 0;
        }
        if (out_bytes != NULL) {
            *out_bytes = dim_m * 4U;
        }
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static int nos_accel_submit_internal(struct nos_accel_device *dev,
                                     struct task_struct *caller,
                                     const struct nos_accel_job_desc *desc,
                                     struct nos_accel_job_result *out_res,
                                     bool already_charged)
{
    struct nos_cap_table *cap_tbl;
    struct nos_cell *cell = NULL;
    struct nos_vmo *w_vmo = NULL;
    struct nos_vmo *in_vmo = NULL;
    struct nos_vmo *out_vmo = NULL;
    struct nos_notification *notif = NULL;
    struct nos_accel_job_result local_res;
    int rc = 0;

    if (dev == NULL || !dev->registered || caller == NULL || desc == NULL) {
        return -EINVAL;
    }
    if (desc->kernel_op >= 64U ||
        (dev->caps.supported_kernels & (1ULL << desc->kernel_op)) == 0ULL) {
        return -ENOTSUP;
    }

    cap_tbl = nos_task_active_cap_table(caller);
    cell = caller->cell;
    if (cap_tbl == NULL) {
        return -EBADF;
    }

    if ((nos_handle_t)desc->weight_vmo_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(cap_tbl, (nos_handle_t)desc->weight_vmo_cap,
                            NOS_CAP_VMO, NOS_CAP_RIGHT_READ, (void **)&w_vmo, NULL);
        if (rc < 0) {
            goto out_put;
        }
    }
    if ((nos_handle_t)desc->input_vmo_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(cap_tbl, (nos_handle_t)desc->input_vmo_cap,
                            NOS_CAP_VMO, NOS_CAP_RIGHT_READ, (void **)&in_vmo, NULL);
        if (rc < 0) {
            goto out_put;
        }
    }
    if ((nos_handle_t)desc->output_vmo_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(cap_tbl, (nos_handle_t)desc->output_vmo_cap,
                            NOS_CAP_VMO, NOS_CAP_RIGHT_WRITE, (void **)&out_vmo, NULL);
        if (rc < 0) {
            goto out_put;
        }
        /* Sealed 只读权重/模型页严禁作为加速器可写输出靶区 */
        if (out_vmo->sealed) {
            rc = -EACCES;
            goto out_put;
        }
    }
    if ((nos_handle_t)desc->notify_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(cap_tbl, (nos_handle_t)desc->notify_cap,
                            NOS_CAP_NOTIFICATION, NOS_CAP_RIGHT_SEND, (void **)&notif, NULL);
        if (rc < 0) {
            goto out_put;
        }
    }

    rc = accel_validate_tensor_bounds(desc, w_vmo, in_vmo, out_vmo);
    if (rc < 0) {
        goto out_put;
    }

    if (cell != NULL) {
        struct nos_iommu_info iinfo;
        struct nos_vm_space *vms = nos_cell_vm_space(cell);
        uint32_t pasid = 0U;

        if (!already_charged) {
            rc = nos_cell_charge_accel_op(cell, (uint64_t)desc->kernel_op, (uint32_t)desc->req_id);
            if (rc < 0) {
                goto out_put;
            }
        }

        if (vms != NULL && vms->initialized) {
            (void)nos_iommu_get_status(&iinfo);
            if (iinfo.max_pasids > 1U) {
                pasid = cell->cell_id % iinfo.max_pasids;
            }
            rc = nos_iommu_bind_space(dev->caps.stream_id, pasid, vms);
            if (rc < 0) {
                goto out_put;
            }
        }

        nos_cell_record_audit(cell,
                              NOS_AUDIT_ACCEL_SUBMIT,
                              (uint64_t)desc->kernel_op,
                              dev->caps.stream_id,
                              0,
                              desc->req_id,
                              (uint64_t)desc->dim_m);
    }

    memset(&local_res, 0, sizeof(local_res));
    local_res.req_id = desc->req_id;
    rc = dev->ops->submit_job(dev, caller, desc, w_vmo, in_vmo, out_vmo, &local_res);
    if (rc == 0) {
        dev->jobs_completed++;
    } else {
        dev->jobs_failed++;
        local_res.status = rc;
    }

    if (out_res != NULL) {
        *out_res = local_res;
    }
    if (rc == 0 && notif != NULL) {
        (void)nos_notify_signal_obj(notif, 1ULL);
    }

out_put:
    if (notif != NULL) {
        (void)nos_object_put(&notif->object);
    }
    if (out_vmo != NULL) {
        (void)nos_object_put(&out_vmo->header);
    }
    if (in_vmo != NULL) {
        (void)nos_object_put(&in_vmo->header);
    }
    if (w_vmo != NULL) {
        (void)nos_object_put(&w_vmo->header);
    }
    return rc;
}

int nos_accel_submit(struct nos_accel_device *dev,
                     struct task_struct *caller,
                     const struct nos_accel_job_desc *desc,
                     struct nos_accel_job_result *out_res)
{
    return nos_accel_submit_internal(dev, caller, desc, out_res, false);
}

int nos_accel_dispatch_ring_sqe(void *accel_obj,
                                struct task_struct *caller,
                                const struct nos_ring_sqe *sqe,
                                struct nos_ring_cqe *out_cqe)
{
    struct nos_accel_device *dev = (struct nos_accel_device *)accel_obj;
    struct nos_accel_job_desc desc;
    struct nos_accel_job_result res;
    int rc;

    if (dev == NULL || caller == NULL || sqe == NULL || out_cqe == NULL) {
        if (dev != NULL) {
            (void)nos_object_put(&dev->header);
        }
        return -EINVAL;
    }

    memset(&desc, 0, sizeof(desc));
    desc.kernel_op = (uint32_t)(sqe->schema_id & 0xFFFFFFFFULL);
    desc.flags = (uint32_t)sqe->flags;
    desc.req_id = sqe->req_id;
    desc.weight_vmo_cap = (uint32_t)(sqe->inline_arg[0] & 0xFFFFFFFFULL);
    desc.output_vmo_cap = (uint32_t)(sqe->inline_arg[0] >> 32);
    desc.input_vmo_cap = sqe->vmo_cap;
    desc.notify_cap = NOS_HANDLE_INVALID;
    desc.input_offset = (uint32_t)sqe->vmo_offset;
    desc.dim_m = (uint32_t)(sqe->inline_arg[1] & 0xFFFFFULL);
    desc.dim_n = (uint32_t)((sqe->inline_arg[1] >> 20) & 0xFFFFFULL);
    desc.dim_k = (uint32_t)((sqe->inline_arg[1] >> 40) & 0xFFFFFULL);
    desc.deadline_ns = ((sqe->flags & NOS_RING_FLAG_DEADLINE) != 0U) ? sqe->deadline_ns : 0ULL;

    memset(&res, 0, sizeof(res));
    /* ring_dispatch_one_sqe 已扣减 accel_ops 配额，此处置 already_charged=true 严防双扣 */
    rc = nos_accel_submit_internal(dev, caller, &desc, &res, true);

    memset(out_cqe, 0, sizeof(*out_cqe));
    out_cqe->req_id = sqe->req_id;
    out_cqe->status = (rc == 0) ? res.status : rc;
    out_cqe->result_len = res.output_bytes;
    out_cqe->elapsed_ns = res.elapsed_ns;

    (void)nos_object_put(&dev->header);
    return rc;
}

#ifdef __HOST_TEST__
void nos_accel_host_reset(void)
{
    uint32_t i;

    accel_ensure_lock();
    spin_lock_irq(&g_accel_list_lock);
    for (i = 0U; i < NOS_ACCEL_MAX_DEVICES; i++) {
        if (g_accel_devs[i] != NULL) {
            g_accel_devs[i]->registered = false;
            g_accel_devs[i] = NULL;
        }
    }
    spin_unlock_irq(&g_accel_list_lock);
}
#endif
