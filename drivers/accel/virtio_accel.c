/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * QEMU VirtIO-Accel 参考加速器与 EL1 混合内核异步计算队列实现
 * - 遵守 -mgeneral-regs-only 零 FP/NEON 污染铁律（纯整数张量算子）
 * - 支持独立 EL1 工作线程 + Notification 异步门铃唤醒 + sched_infer 截止期继承
 */

#define pr_fmt(fmt) "[VIRTIO-ACCEL]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/accel.h>
#include <nos/completion.h>
#include <nos/hal_arch.h>
#include <nos/notification.h>
#include <nos/sched_infer.h>
#include <nos/vmo.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <string.h>

#ifdef __HOST_TEST__
#include <stdlib.h>
#ifndef pr_info
#define pr_info(...) ((void)0)
#endif
#else
#ifdef CONFIG_ARCH_ARM64
#include <asm/fdt_scan.h>
#include <asm/mmu.h>
#endif
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#endif

#define VIRTIO_ACCEL_QUEUE_CAP  16U
#define VIRTIO_ACCEL_STREAM_ID  1U
#define VIRTIO_ACCEL_STACK_BUF  512U

struct virtio_accel_slot {
    struct nos_accel_job_desc   desc;
    struct nos_vmo             *weight_vmo;
    struct nos_vmo             *input_vmo;
    struct nos_vmo             *output_vmo;
    struct nos_accel_job_result result;
    struct nos_completion       done;
    uint64_t                    abs_deadline_ns;
    int                         rc;
};

struct virtio_accel_state {
    spinlock_t                lock;
    struct nos_notification   queue_notif;
    struct virtio_accel_slot *queue[VIRTIO_ACCEL_QUEUE_CAP];
    uint32_t                  head;
    uint32_t                  tail;
    uint32_t                  count;
    struct task_struct       *worker_task;
    bool                      worker_started;
    bool                      mmio_probed;
    uint32_t                  mmio_magic;
    uint32_t                  mmio_device_id;
};

static struct nos_accel_device g_virtio_accel_dev;
static struct virtio_accel_state g_virtio_state;

static int accel_read_vmo_buf(struct nos_vmo *vmo, uint32_t off, uint32_t len,
                              uint8_t *stack_buf, void **out_ptr, bool *out_heap)
{
    void *buf = stack_buf;
    int rc;

    *out_ptr = NULL;
    *out_heap = false;
    if (vmo == NULL || len == 0U) {
        return 0;
    }
    if (len > VIRTIO_ACCEL_STACK_BUF) {
        buf = kmalloc(len, GFP_KERNEL);
        if (buf == NULL) {
            return -ENOMEM;
        }
        *out_heap = true;
    }
    rc = nos_vmo_read(vmo, (size_t)off, buf, (size_t)len);
    if (rc != 0) {
        if (*out_heap) {
            kfree(buf);
        }
        *out_heap = false;
        return rc;
    }
    *out_ptr = buf;
    return 0;
}

static void accel_calc_io_bytes(const struct nos_accel_job_desc *desc,
                                uint32_t *w_bytes,
                                uint32_t *in_bytes,
                                uint32_t *out_bytes)
{
    *w_bytes = 0U;
    *in_bytes = 0U;
    *out_bytes = 0U;

    switch (desc->kernel_op) {
    case NOS_ACCEL_KERNEL_GEMV_I32:
        *w_bytes = desc->dim_m * desc->dim_k * 4U;
        *in_bytes = desc->dim_k * 4U;
        *out_bytes = desc->dim_m * 4U;
        break;
    case NOS_ACCEL_KERNEL_MATMUL_I8:
        *w_bytes = desc->dim_k * desc->dim_n;
        *in_bytes = desc->dim_m * desc->dim_k;
        *out_bytes = desc->dim_m * desc->dim_n * 4U;
        break;
    case NOS_ACCEL_KERNEL_RELU_I32:
        *in_bytes = desc->dim_m * 4U;
        *out_bytes = desc->dim_m * 4U;
        break;
    default:
        break;
    }
}

static int virtio_execute_slot(struct virtio_accel_slot *slot)
{
    uint8_t w_stack[VIRTIO_ACCEL_STACK_BUF];
    uint8_t in_stack[VIRTIO_ACCEL_STACK_BUF];
    uint8_t out_stack[VIRTIO_ACCEL_STACK_BUF];
    void *w_ptr = NULL;
    void *in_ptr = NULL;
    void *out_ptr = out_stack;
    bool w_heap = false;
    bool in_heap = false;
    bool out_heap = false;
    uint32_t w_len = 0U;
    uint32_t in_len = 0U;
    uint32_t out_len = 0U;
    uint32_t produced = 0U;
    uint64_t t0;
    int rc = 0;

    t0 = hal_timer_monotonic_ns();
    accel_calc_io_bytes(&slot->desc, &w_len, &in_len, &out_len);

    rc = accel_read_vmo_buf(slot->weight_vmo, slot->desc.weight_offset, w_len,
                            w_stack, &w_ptr, &w_heap);
    if (rc != 0) {
        goto out;
    }

    rc = accel_read_vmo_buf(slot->input_vmo, slot->desc.input_offset, in_len,
                            in_stack, &in_ptr, &in_heap);
    if (rc != 0) {
        goto out;
    }

    if (out_len > VIRTIO_ACCEL_STACK_BUF) {
        out_ptr = kmalloc(out_len, GFP_KERNEL);
        if (out_ptr == NULL) {
            rc = -ENOMEM;
            goto out;
        }
        out_heap = true;
    }

    rc = nos_accel_exec_cpu_kernel(slot->desc.kernel_op, w_ptr, in_ptr, out_ptr,
                                   slot->desc.dim_m, slot->desc.dim_n, slot->desc.dim_k,
                                   &produced);
    if (rc == 0 && produced > 0U && slot->output_vmo != NULL) {
        /* 经由 nos_vmo_write 写入结果，自动维持 CoW 快照与只读封存一致性 */
        rc = nos_vmo_write(slot->output_vmo, (size_t)slot->desc.output_offset,
                           out_ptr, (size_t)produced);
    }

    slot->result.req_id = slot->desc.req_id;
    slot->result.status = rc;
    slot->result.output_bytes = (rc == 0) ? produced : 0U;
    slot->result.elapsed_ns = hal_timer_monotonic_ns() - t0;

out:
    if (w_heap) {
        kfree(w_ptr);
    }
    if (in_heap) {
        kfree(in_ptr);
    }
    if (out_heap) {
        kfree(out_ptr);
    }
    return rc;
}

#ifndef __HOST_TEST__
static void virtio_accel_worker_entry(void *param)
{
    struct virtio_accel_state *st = (struct virtio_accel_state *)param;

    for (;;) {
        struct virtio_accel_slot *slot = NULL;
        uint64_t bits = 0ULL;
        bool boosted;

        spin_lock_irq(&st->lock);
        if (st->count > 0U) {
            slot = st->queue[st->head];
            st->queue[st->head] = NULL;
            st->head = (st->head + 1U) % VIRTIO_ACCEL_QUEUE_CAP;
            st->count--;
        }
        spin_unlock_irq(&st->lock);

        if (slot == NULL) {
            int wrc = nos_notify_wait_obj(&st->queue_notif, &bits, NOS_WAIT_FOREVER);
            if (wrc < 0 && wrc != -EAGAIN) {
                break;
            }
            continue;
        }

        boosted = (slot->abs_deadline_ns != 0ULL);
        slot->rc = virtio_execute_slot(slot);
        if (boosted && current != NULL) {
            sched_infer_restore_deadline(current);
        }
        (void)nos_complete(&slot->done);
    }
}

static int virtio_ensure_worker_started(void)
{
    struct task_struct *t = NULL;

    spin_lock_irq(&g_virtio_state.lock);
    if (g_virtio_state.worker_started && g_virtio_state.worker_task != NULL) {
        spin_unlock_irq(&g_virtio_state.lock);
        return 0;
    }
    spin_unlock_irq(&g_virtio_state.lock);

    /* 必须以 SCHED_POLICY_NORMAL 启动工作线程，支持提交侧截止期继承 (Deadline Inheritance) */
    t = task_create_ex("virtio_accel", virtio_accel_worker_entry, &g_virtio_state,
                       SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                       CONFIG_CORE_TASK_STACK_SIZE, 10U, NULL);
    if (t == NULL) {
        return -ENOMEM;
    }

    spin_lock_irq(&g_virtio_state.lock);
    if (g_virtio_state.worker_started && g_virtio_state.worker_task != NULL) {
        spin_unlock_irq(&g_virtio_state.lock);
        task_del(t);
        return 0;
    }
    g_virtio_state.worker_task = t;
    g_virtio_state.worker_started = true;
    spin_unlock_irq(&g_virtio_state.lock);

    (void)task_ready(t);
    return 0;
}
#endif

static int virtio_query_caps(struct nos_accel_device *dev, struct nos_accel_caps *out_caps)
{
    *out_caps = dev->caps;
    return 0;
}

static int virtio_submit_job(struct nos_accel_device *dev,
                             struct task_struct *caller,
                             const struct nos_accel_job_desc *desc,
                             struct nos_vmo *weight_vmo,
                             struct nos_vmo *input_vmo,
                             struct nos_vmo *output_vmo,
                             struct nos_accel_job_result *out_res)
{
    struct virtio_accel_slot slot;

    (void)dev;
    (void)caller;
    memset(&slot, 0, sizeof(slot));
    slot.desc = *desc;
    slot.weight_vmo = weight_vmo;
    slot.input_vmo = input_vmo;
    slot.output_vmo = output_vmo;

#ifndef __HOST_TEST__
    if (kernel_running && !hal_in_irq() && virtio_ensure_worker_started() == 0) {
        struct task_struct *worker = NULL;
        uint64_t dl_abs = 0ULL;

        (void)nos_completion_init(&slot.done);
        if (desc->deadline_ns > 0ULL) {
            uint64_t now_ns = hal_timer_monotonic_ns();
            dl_abs = (desc->deadline_ns <= 100000000000ULL) ?
                     (now_ns + desc->deadline_ns) : desc->deadline_ns;
            slot.abs_deadline_ns = dl_abs;
        }

        spin_lock_irq(&g_virtio_state.lock);
        if (g_virtio_state.count >= VIRTIO_ACCEL_QUEUE_CAP) {
            spin_unlock_irq(&g_virtio_state.lock);
            (void)nos_object_close(&slot.done.object);
            (void)nos_object_retire(&slot.done.object);
            return -EBUSY;
        }
        g_virtio_state.queue[g_virtio_state.tail] = &slot;
        g_virtio_state.tail = (g_virtio_state.tail + 1U) % VIRTIO_ACCEL_QUEUE_CAP;
        g_virtio_state.count++;
        if (dl_abs != 0ULL && g_virtio_state.worker_task != NULL &&
            task_get(g_virtio_state.worker_task)) {
            worker = g_virtio_state.worker_task;
        }
        spin_unlock_irq(&g_virtio_state.lock);

        if (worker != NULL) {
            (void)sched_infer_boost_deadline(worker, dl_abs);
            (void)task_put(worker);
        }
        (void)nos_notify_signal_obj(&g_virtio_state.queue_notif, 1ULL);
        (void)nos_completion_wait(&slot.done, NOS_WAIT_FOREVER);
        (void)nos_object_close(&slot.done.object);
        (void)nos_object_retire(&slot.done.object);
        if (out_res != NULL) {
            *out_res = slot.result;
        }
        return slot.rc;
    }
#endif

    slot.rc = virtio_execute_slot(&slot);
    if (out_res != NULL) {
        *out_res = slot.result;
    }
    return slot.rc;
}

static const struct nos_accel_ops g_virtio_ops = {
    .query_caps = virtio_query_caps,
    .bind_vmo   = NULL,
    .submit_job = virtio_submit_job,
    .cancel_job = NULL,
    .reset_ctx  = NULL,
};

int virtio_accel_init(void)
{
    struct nos_accel_caps caps;

    memset(&g_virtio_state, 0, sizeof(g_virtio_state));
    spin_lock_init(&g_virtio_state.lock);
    (void)nos_notification_init(&g_virtio_state.queue_notif);

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    {
        uint64_t mmio_pa = 0ULL;
        uint64_t mmio_sz = 0ULL;
        if (arm64_fdt_find_reg("virtio,mmio", 0U, &mmio_pa, &mmio_sz) == 0 && mmio_pa != 0ULL) {
            volatile uint32_t *vmmio = (volatile uint32_t *)arm64_ioremap(
                (uintptr_t)mmio_pa, (mmio_sz != 0ULL) ? (size_t)mmio_sz : 0x200U);
            if (vmmio != NULL) {
                g_virtio_state.mmio_magic = vmmio[0];
                g_virtio_state.mmio_device_id = vmmio[2];
                g_virtio_state.mmio_probed = true;
            }
        }
    }
#endif

    memset(&caps, 0, sizeof(caps));
    caps.accel_type = NOS_ACCEL_TYPE_VIRTIO;
    caps.core_count = 4U;
    caps.stream_id = VIRTIO_ACCEL_STREAM_ID;
    caps.flags = NOS_ACCEL_CAP_SVA | NOS_ACCEL_CAP_ASYNC_QUEUE |
                 NOS_ACCEL_CAP_INT8_MATMUL | NOS_ACCEL_CAP_INT32_GEMV;
    caps.supported_kernels = (1ULL << NOS_ACCEL_KERNEL_NOP) |
                             (1ULL << NOS_ACCEL_KERNEL_MATMUL_I8) |
                             (1ULL << NOS_ACCEL_KERNEL_GEMV_I32) |
                             (1ULL << NOS_ACCEL_KERNEL_RELU_I32);
    caps.peak_tops_x100 = 400ULL;
    caps.max_tensor_bytes = 64ULL * 1024ULL * 1024ULL;
    memcpy(caps.name, "virtio-accel-el1", 17U);

    if (nos_accel_register(&g_virtio_accel_dev, &caps, &g_virtio_ops, &g_virtio_state) != 0) {
        return -EIO;
    }

    pr_info("VirtIO-Accel registered (stream_id=%u, mmio_probed=%d, magic=0x%x)\n",
            (unsigned int)caps.stream_id,
            g_virtio_state.mmio_probed ? 1 : 0,
            (unsigned int)g_virtio_state.mmio_magic);
    return 0;
}

#ifndef __HOST_TEST__
SYS_INIT(virtio_accel_init, INIT_LEVEL_POST_KERNEL);
#endif
