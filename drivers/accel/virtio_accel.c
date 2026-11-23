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
    uint32_t                    pasid;
    uint32_t                    waiters;
    bool                        in_use;
    bool                        is_async;
    bool                        running;
    bool                        completing;
    bool                        executed;
    bool                        canceled;
    bool                        auto_reap;
};

struct virtio_accel_state {
    spinlock_t                lock;
    struct nos_notification   queue_notif;
    struct virtio_accel_slot *queue[VIRTIO_ACCEL_QUEUE_CAP];
    struct virtio_accel_slot  async_pool[VIRTIO_ACCEL_QUEUE_CAP];
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

static void virtio_slot_retain_vmos(struct virtio_accel_slot *slot)
{
    if (slot == NULL) {
        return;
    }
    if (slot->weight_vmo != NULL) {
        (void)nos_object_get(&slot->weight_vmo->header);
    }
    if (slot->input_vmo != NULL) {
        (void)nos_object_get(&slot->input_vmo->header);
    }
    if (slot->output_vmo != NULL) {
        (void)nos_object_get(&slot->output_vmo->header);
    }
}

static void virtio_slot_release_vmos(struct virtio_accel_slot *slot)
{
    if (slot == NULL) {
        return;
    }
    if (slot->weight_vmo != NULL) {
        (void)nos_object_put(&slot->weight_vmo->header);
        slot->weight_vmo = NULL;
    }
    if (slot->input_vmo != NULL) {
        (void)nos_object_put(&slot->input_vmo->header);
        slot->input_vmo = NULL;
    }
    if (slot->output_vmo != NULL) {
        (void)nos_object_put(&slot->output_vmo->header);
        slot->output_vmo = NULL;
    }
}

/* 必须在持有 st->lock 时调用 */
static bool virtio_queue_remove_locked(struct virtio_accel_state *st,
                                       const struct virtio_accel_slot *target)
{
    struct virtio_accel_slot *kept[VIRTIO_ACCEL_QUEUE_CAP];
    uint32_t k = 0U;
    bool removed = false;

    for (uint32_t i = 0U; i < st->count; i++) {
        uint32_t idx = (st->head + i) % VIRTIO_ACCEL_QUEUE_CAP;
        struct virtio_accel_slot *s = st->queue[idx];
        st->queue[idx] = NULL;
        if (s == target) {
            removed = true;
        } else if (s != NULL) {
            kept[k++] = s;
        }
    }

    st->head = 0U;
    st->tail = k % VIRTIO_ACCEL_QUEUE_CAP;
    st->count = k;
    for (uint32_t i = 0U; i < k; i++) {
        st->queue[i] = kept[i];
    }
    return removed;
}

static void virtio_reap_async_slot_unlocked(struct virtio_accel_slot *slot)
{
#ifndef __HOST_TEST__
    if (slot->is_async) {
        (void)nos_object_close(&slot->done.object);
        (void)nos_object_retire(&slot->done.object);
    }
#endif
    virtio_slot_release_vmos(slot);
}

#ifndef __HOST_TEST__
static void virtio_accel_worker_entry(void *param)
{
    struct virtio_accel_state *st = (struct virtio_accel_state *)param;

    for (;;) {
        struct virtio_accel_slot *slot = NULL;
        uint64_t bits = 0ULL;
        bool boosted;
        bool skip_exec = false;
        bool notify_done = false;
        bool reap_now = false;

        spin_lock_irq(&st->lock);
        if (st->count > 0U) {
            slot = st->queue[st->head];
            st->queue[st->head] = NULL;
            st->head = (st->head + 1U) % VIRTIO_ACCEL_QUEUE_CAP;
            st->count--;
            if (slot != NULL) {
                skip_exec = slot->canceled;
                if (!skip_exec) {
                    slot->running = true;
                }
            }
        }
        spin_unlock_irq(&st->lock);

        if (slot == NULL) {
            int wrc = nos_notify_wait_obj(&st->queue_notif, &bits, NOS_WAIT_FOREVER);
            if (wrc < 0 && wrc != -EAGAIN) {
                break;
            }
            continue;
        }

        if (skip_exec) {
            slot->rc = -ECANCELED;
            slot->result.req_id = slot->desc.req_id;
            slot->result.status = -ECANCELED;
            slot->result.output_bytes = 0U;
            spin_lock_irq(&st->lock);
            slot->executed = true;
            if (slot->is_async && slot->auto_reap && slot->waiters == 0U) {
                reap_now = true;
            } else {
                notify_done = true;
                slot->completing = true;
            }
            spin_unlock_irq(&st->lock);
        } else {
            int exec_rc;
            boosted = (slot->abs_deadline_ns != 0ULL);
            exec_rc = virtio_execute_slot(slot);
            if (boosted && current != NULL) {
                sched_infer_restore_deadline(current);
            }
            spin_lock_irq(&st->lock);
            if (slot->canceled) {
                slot->rc = -ECANCELED;
                slot->result.req_id = slot->desc.req_id;
                slot->result.status = -ECANCELED;
                slot->result.output_bytes = 0U;
            } else {
                slot->rc = exec_rc;
            }
            slot->running = false;
            slot->executed = true;
            if (slot->is_async && slot->auto_reap && slot->waiters == 0U) {
                reap_now = true;
            } else {
                notify_done = true;
                slot->completing = true;
            }
            spin_unlock_irq(&st->lock);
        }

        if (notify_done) {
            (void)nos_complete(&slot->done);
            spin_lock_irq(&st->lock);
            slot->completing = false;
            if (slot->is_async && slot->auto_reap && slot->waiters == 0U) {
                reap_now = true;
            }
            spin_unlock_irq(&st->lock);
        }

        if (reap_now) {
            virtio_reap_async_slot_unlocked(slot);
            spin_lock_irq(&st->lock);
            memset(slot, 0, sizeof(*slot));
            spin_unlock_irq(&st->lock);
        }
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
    bool is_async;

    (void)dev;
    (void)caller;
    if (desc == NULL) {
        return -EINVAL;
    }

    is_async = ((desc->flags & NOS_ACCEL_FLAG_ASYNC) != 0U);
    if (is_async) {
        struct virtio_accel_slot *slot = NULL;
        bool use_worker = false;
        int rc = 0;

#ifndef __HOST_TEST__
        if (kernel_running && !hal_in_irq() && virtio_ensure_worker_started() == 0) {
            use_worker = true;
        }
#endif

        spin_lock_irq(&g_virtio_state.lock);
        for (uint32_t i = 0U; i < VIRTIO_ACCEL_QUEUE_CAP; i++) {
            if (g_virtio_state.async_pool[i].in_use &&
                g_virtio_state.async_pool[i].desc.req_id == desc->req_id) {
                spin_unlock_irq(&g_virtio_state.lock);
                return -EEXIST;
            }
            if (!g_virtio_state.async_pool[i].in_use && slot == NULL) {
                slot = &g_virtio_state.async_pool[i];
            }
        }
        if (slot == NULL || (use_worker && g_virtio_state.count >= VIRTIO_ACCEL_QUEUE_CAP)) {
            spin_unlock_irq(&g_virtio_state.lock);
            return -EBUSY;
        }

        memset(slot, 0, sizeof(*slot));
        slot->desc = *desc;
        slot->weight_vmo = weight_vmo;
        slot->input_vmo = input_vmo;
        slot->output_vmo = output_vmo;
        slot->rc = -EINPROGRESS;
        slot->result.req_id = desc->req_id;
        slot->result.status = -EINPROGRESS;
        slot->in_use = true;
        slot->is_async = true;
        virtio_slot_retain_vmos(slot);
        spin_unlock_irq(&g_virtio_state.lock);

#ifndef __HOST_TEST__
        if (use_worker) {
            struct task_struct *worker = NULL;
            uint64_t dl_abs = 0ULL;

            (void)nos_completion_init(&slot->done);
            if (desc->deadline_ns > 0ULL) {
                uint64_t now_ns = hal_timer_monotonic_ns();
                dl_abs = (desc->deadline_ns <= 100000000000ULL) ?
                         (now_ns + desc->deadline_ns) : desc->deadline_ns;
                slot->abs_deadline_ns = dl_abs;
            }

            spin_lock_irq(&g_virtio_state.lock);
            if (slot->canceled) {
                spin_unlock_irq(&g_virtio_state.lock);
                virtio_reap_async_slot_unlocked(slot);
                spin_lock_irq(&g_virtio_state.lock);
                memset(slot, 0, sizeof(*slot));
                spin_unlock_irq(&g_virtio_state.lock);
                return -ECANCELED;
            }
            if (g_virtio_state.count >= VIRTIO_ACCEL_QUEUE_CAP) {
                spin_unlock_irq(&g_virtio_state.lock);
                virtio_reap_async_slot_unlocked(slot);
                spin_lock_irq(&g_virtio_state.lock);
                memset(slot, 0, sizeof(*slot));
                spin_unlock_irq(&g_virtio_state.lock);
                return -EBUSY;
            }
            g_virtio_state.queue[g_virtio_state.tail] = slot;
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
        }
#endif

        if (out_res != NULL) {
            memset(out_res, 0, sizeof(*out_res));
            out_res->req_id = desc->req_id;
            out_res->status = -EINPROGRESS;
        }
        return rc;
    }

    {
        struct virtio_accel_slot slot;

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
            spin_lock_irq(&g_virtio_state.lock);
            while (slot.completing) {
                spin_unlock_irq(&g_virtio_state.lock);
                spin_lock_irq(&g_virtio_state.lock);
            }
            spin_unlock_irq(&g_virtio_state.lock);
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
}

static int virtio_wait_job(struct nos_accel_device *dev,
                           uint64_t req_id,
                           struct nos_accel_job_result *out_res,
                           uint32_t timeout_ms)
{
    struct virtio_accel_slot *slot = NULL;
    struct nos_accel_job_result local_res;
    bool already_done = false;
    bool canceled = false;
    bool need_sync_exec = false;
    int rc = 0;

    (void)dev;
    (void)timeout_ms;
    memset(&local_res, 0, sizeof(local_res));
    local_res.req_id = req_id;

    spin_lock_irq(&g_virtio_state.lock);
    for (uint32_t i = 0U; i < VIRTIO_ACCEL_QUEUE_CAP; i++) {
        if (g_virtio_state.async_pool[i].in_use &&
            g_virtio_state.async_pool[i].is_async &&
            g_virtio_state.async_pool[i].desc.req_id == req_id) {
            slot = &g_virtio_state.async_pool[i];
            break;
        }
    }
    if (slot == NULL) {
        spin_unlock_irq(&g_virtio_state.lock);
        return -ENOENT;
    }

    if (slot->canceled) {
        canceled = true;
        if (slot->running || slot->completing || slot->waiters > 0U) {
            slot->auto_reap = true;
            spin_unlock_irq(&g_virtio_state.lock);
            if (out_res != NULL) {
                local_res.status = -ECANCELED;
                *out_res = local_res;
            }
            return -ECANCELED;
        }
        local_res = slot->result;
        local_res.status = -ECANCELED;
        spin_unlock_irq(&g_virtio_state.lock);
        goto reap_return;
    }

    if (slot->executed && !slot->running && !slot->completing) {
        already_done = true;
        rc = slot->rc;
        local_res = slot->result;
        spin_unlock_irq(&g_virtio_state.lock);
        goto reap_return;
    }

#ifdef __HOST_TEST__
    slot->running = true;
    need_sync_exec = true;
    spin_unlock_irq(&g_virtio_state.lock);
#else
    if (!g_virtio_state.worker_started) {
        slot->running = true;
        need_sync_exec = true;
        spin_unlock_irq(&g_virtio_state.lock);
    } else {
        nos_timeout_t ticks;
        int wrc;

        slot->waiters++;
        spin_unlock_irq(&g_virtio_state.lock);
        if (timeout_ms == UINT32_MAX) {
            ticks = NOS_WAIT_FOREVER;
        } else if (timeout_ms == 0U) {
            ticks = NOS_NO_WAIT;
        } else {
            ticks = (nos_timeout_t)timeout_ms;
        }
        wrc = nos_completion_wait(&slot->done, ticks);

        spin_lock_irq(&g_virtio_state.lock);
        if (slot->waiters > 0U) {
            slot->waiters--;
        }
        if (wrc == -EAGAIN || wrc == -ETIMEDOUT) {
            if (!slot->canceled || slot->running || slot->completing || slot->waiters > 0U) {
                spin_unlock_irq(&g_virtio_state.lock);
                return wrc;
            }
            canceled = true;
            local_res = slot->result;
            local_res.status = -ECANCELED;
            spin_unlock_irq(&g_virtio_state.lock);
            goto reap_return;
        }
        while (slot->running || slot->completing) {
            spin_unlock_irq(&g_virtio_state.lock);
            spin_lock_irq(&g_virtio_state.lock);
        }
        canceled = slot->canceled;
        if (slot->waiters > 0U) {
            rc = canceled ? -ECANCELED : slot->rc;
            local_res = slot->result;
            slot->auto_reap = true;
            spin_unlock_irq(&g_virtio_state.lock);
            if (out_res != NULL) {
                if (canceled) {
                    local_res.status = -ECANCELED;
                }
                *out_res = local_res;
            }
            return rc;
        }
        rc = canceled ? -ECANCELED : slot->rc;
        local_res = slot->result;
        if (canceled) {
            local_res.status = -ECANCELED;
        }
        spin_unlock_irq(&g_virtio_state.lock);
        goto reap_return;
    }
#endif

    if (need_sync_exec) {
        rc = virtio_execute_slot(slot);
        spin_lock_irq(&g_virtio_state.lock);
        slot->running = false;
        slot->executed = true;
        if (slot->canceled) {
            canceled = true;
            rc = -ECANCELED;
            slot->rc = -ECANCELED;
            slot->result.status = -ECANCELED;
        } else {
            slot->rc = rc;
        }
        local_res = slot->result;
        spin_unlock_irq(&g_virtio_state.lock);
    }

reap_return:
    (void)already_done;
    virtio_reap_async_slot_unlocked(slot);
    spin_lock_irq(&g_virtio_state.lock);
    memset(slot, 0, sizeof(*slot));
    spin_unlock_irq(&g_virtio_state.lock);

    if (out_res != NULL) {
        *out_res = local_res;
    }
    return canceled ? -ECANCELED : rc;
}

static int virtio_cancel_job(struct nos_accel_device *dev, uint64_t req_id)
{
    struct virtio_accel_slot *slot = NULL;
    bool wait_worker = false;

    (void)dev;
    spin_lock_irq(&g_virtio_state.lock);
    for (uint32_t i = 0U; i < VIRTIO_ACCEL_QUEUE_CAP; i++) {
        if (g_virtio_state.async_pool[i].in_use &&
            g_virtio_state.async_pool[i].desc.req_id == req_id) {
            slot = &g_virtio_state.async_pool[i];
            break;
        }
    }
    if (slot == NULL) {
        spin_unlock_irq(&g_virtio_state.lock);
        return -ENOENT;
    }

    slot->canceled = true;
    slot->rc = -ECANCELED;
    slot->result.req_id = req_id;
    slot->result.status = -ECANCELED;
    slot->result.output_bytes = 0U;
    (void)virtio_queue_remove_locked(&g_virtio_state, slot);

    if (slot->running || slot->completing || slot->waiters > 0U) {
        slot->auto_reap = true;
        wait_worker = true;
    }
    spin_unlock_irq(&g_virtio_state.lock);

#ifndef __HOST_TEST__
    if (slot->is_async && wait_worker) {
        (void)nos_complete_all(&slot->done);
    }
#endif

    if (!wait_worker) {
        virtio_reap_async_slot_unlocked(slot);
        spin_lock_irq(&g_virtio_state.lock);
        memset(slot, 0, sizeof(*slot));
        spin_unlock_irq(&g_virtio_state.lock);
    }
    return 0;
}

static int virtio_reset_ctx(struct nos_accel_device *dev, uint32_t pasid)
{
    struct virtio_accel_slot *reap_list[VIRTIO_ACCEL_QUEUE_CAP];
    struct virtio_accel_slot *wake_list[VIRTIO_ACCEL_QUEUE_CAP];
    uint32_t reap_cnt = 0U;
    uint32_t wake_cnt = 0U;

    (void)dev;
    spin_lock_irq(&g_virtio_state.lock);
    for (uint32_t i = 0U; i < VIRTIO_ACCEL_QUEUE_CAP; i++) {
        struct virtio_accel_slot *slot = &g_virtio_state.async_pool[i];
        if (!slot->in_use) {
            continue;
        }
        if (pasid != 0U && slot->pasid != pasid) {
            continue;
        }
        slot->canceled = true;
        slot->rc = -ECANCELED;
        slot->result.status = -ECANCELED;
        (void)virtio_queue_remove_locked(&g_virtio_state, slot);
        if (slot->running || slot->completing || slot->waiters > 0U) {
            slot->auto_reap = true;
            if (slot->is_async) {
                wake_list[wake_cnt++] = slot;
            }
        } else {
            reap_list[reap_cnt++] = slot;
        }
    }
    spin_unlock_irq(&g_virtio_state.lock);

#ifndef __HOST_TEST__
    for (uint32_t i = 0U; i < wake_cnt; i++) {
        (void)nos_complete_all(&wake_list[i]->done);
    }
#else
    (void)wake_list;
    (void)wake_cnt;
#endif

    for (uint32_t i = 0U; i < reap_cnt; i++) {
        virtio_reap_async_slot_unlocked(reap_list[i]);
        spin_lock_irq(&g_virtio_state.lock);
        memset(reap_list[i], 0, sizeof(*reap_list[i]));
        spin_unlock_irq(&g_virtio_state.lock);
    }
    return 0;
}

static const struct nos_accel_ops g_virtio_ops = {
    .query_caps = virtio_query_caps,
    .bind_vmo   = NULL,
    .submit_job = virtio_submit_job,
    .cancel_job = virtio_cancel_job,
    .wait_job   = virtio_wait_job,
    .reset_ctx  = virtio_reset_ctx,
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
