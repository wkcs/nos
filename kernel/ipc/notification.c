/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 64 位异步位图通知 (nos_notify) 核心实现
 * 专用于硬件外设中断向工作线程发送异步事件标志，支持高频同位合并。
 */

#define pr_fmt(fmt) "[NOTIFY]:%s[%d]:" fmt, __func__, __LINE__

#include <nos/notification.h>
#include <nos/ipc.h>
#include <nos/cap.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/irq.h>
#include <kernel/printk.h>

/* =========================================================================
 * 64 位异步位图通知对象核心操作
 * ========================================================================= */

int nos_notification_init(struct nos_notification *notif)
{
    int rc;

    if (notif == NULL) {
        return -EINVAL;
    }

    rc = nos_object_init(&notif->object, true);
    if (rc != 0) {
        return rc;
    }

    notif->pending_bits = 0ULL;
    nos_wait_queue_init(&notif->wait_queue);

    return 0;
}

int nos_notification_close(struct nos_notification *notif)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;

    if (notif == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&notif->object.lock);

    /* 幂等性：若已关闭直接返回 0 */
    if (notif->object.closed) {
        spin_unlock_irq(&notif->object.lock);
        return 0;
    }

    notif->object.closed = true;

    /* 唤醒等待队列中的所有等待者，并赋返回值 -EPIPE */
    while (!nos_wait_queue_empty(&notif->wait_queue)) {
        entry = nos_wait_queue_first(&notif->wait_queue);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    spin_unlock_irq(&notif->object.lock);

    /* 在对象锁外安全分发唤醒批次，避免反向锁序 */
    nos_wake_batch_dispatch(&batch);

    return 0;
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;

    if (notif == NULL || bits == 0ULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&notif->object.lock);

    /* 校验对象是否已关闭 */
    if (notif->object.closed) {
        spin_unlock_irq(&notif->object.lock);
        return -EPIPE;
    }

    /* 累积位图事件 (天然去抖与同位合并) */
    notif->pending_bits |= bits;

    /* 若有等待任务，直接将全部累积位图交付给最高优先级的等待任务 */
    if (!nos_wait_queue_empty(&notif->wait_queue)) {
        entry = nos_wait_queue_first(&notif->wait_queue);
        if (entry != NULL && entry->task != NULL) {
            entry->task->notify_bits = notif->pending_bits;
            notif->pending_bits = 0ULL;
            (void)nos_wait_complete_locked(entry, 0, &batch);
        }
        spin_unlock_irq(&notif->object.lock);

        /* 在对象锁外分发唤醒批次 */
        nos_wake_batch_dispatch(&batch);
        return 0;
    }

    spin_unlock_irq(&notif->object.lock);
    return 0;
}

int nos_notify_wait_obj(struct nos_notification *notif,
                        uint64_t *out_bits,
                        nos_timeout_t timeout)
{
    struct task_struct *task;
    uint64_t deadline;
    int rc;

    if (notif == NULL || out_bits == NULL) {
        return -EINVAL;
    }

    task = current;

    /* 无当前任务上下文（早期启动或非任务环境）仅支持非阻塞获取 */
    if (task == NULL) {
        spin_lock_irq(&notif->object.lock);
        if (notif->object.closed) {
            spin_unlock_irq(&notif->object.lock);
            return -EPIPE;
        }
        if (notif->pending_bits != 0ULL) {
            *out_bits = notif->pending_bits;
            notif->pending_bits = 0ULL;
            spin_unlock_irq(&notif->object.lock);
            return 0;
        }
        spin_unlock_irq(&notif->object.lock);
        return -EAGAIN;
    }

    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't wait notification\n");
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&notif->object.lock);

    /* 校验对象是否已关闭 */
    if (notif->object.closed) {
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return -EPIPE;
    }

    /* 快路径：若已有累积位图，原子读清并返回 */
    if (notif->pending_bits != 0ULL) {
        *out_bits = notif->pending_bits;
        notif->pending_bits = 0ULL;
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    /* 位图为空且指定非阻塞时，立即返回 -EAGAIN */
    if (timeout == NOS_NO_WAIT) {
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return -EAGAIN;
    }

    /* 慢路径：挂入等待队列阻塞休眠 */
    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    rc = nos_wait_prepare(&task->wait_entry, &notif->object, deadline);
    if (rc < 0) {
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    task->notify_bits = 0ULL;
    nos_wait_queue_add_prio(&notif->wait_queue, &task->wait_entry);
    task_set_wchan(task, "nos_notify", (uintptr_t)notif);

    rc = task_hang_lock(task);
    if (rc < 0) {
        nos_wait_queue_del(&notif->wait_queue, &task->wait_entry);
        nos_wait_finish(&task->wait_entry);
        task_clear_wchan(task);
        spin_unlock_irq(&notif->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    /* 若设置了有限超时，启动任务定时器 */
    if (timeout != NOS_WAIT_FOREVER) {
        timer_start(&task->timer, (uint32_t)timeout);
    }

    spin_unlock_irq(&notif->object.lock);
    spin_unlock_irq(&task->lock);

    /* 切换调度，当前任务让出 CPU 阻塞休眠 */
    switch_task();

    /*
     * 任务被唤醒后（可能由 signal 交付唤醒、close 唤醒，或由 timer 定时器超时唤醒）：
     * 在对象锁内执行单点状态机终态确认。
     */
    spin_lock_irq(&notif->object.lock);
    if (task->wait_entry.state == NOS_WAIT_STATE_WAITING) {
        (void)nos_wait_complete_locked(&task->wait_entry, -ETIMEDOUT, NULL);
    }
    rc = task->wait_entry.result;
    spin_unlock_irq(&notif->object.lock);

    /* 停止可能仍在运行的定时器并清理状态 */
    if (timeout != NOS_WAIT_FOREVER) {
        timer_stop(&task->timer);
    }
    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
    }
    task_clear_wchan(task);

    /* 结束等待，清理节点关联并释放持有的对象引用 */
    nos_wait_finish(&task->wait_entry);

    if (rc == 0) {
        *out_bits = task->notify_bits;
        task->notify_bits = 0ULL;
        return 0;
    }

    return rc;
}

/* =========================================================================
 * Capability 句柄包装与动态创建/销毁
 * ========================================================================= */

static void nos_notification_free_release(struct nos_object *obj)
{
    struct nos_notification *notif = container_of(obj, struct nos_notification, object);
    kfree(notif);
}

void nos_notification_destroy(struct nos_notification *notif)
{
    if (notif == NULL) {
        return;
    }

    nos_notification_close(notif);
    if (!notif->object.is_static) {
        nos_object_retire(&notif->object);
    }
}

struct nos_notification *nos_notification_create(nos_handle_t *out_cap)
{
    struct nos_notification *notif;
    struct task_struct *task = current;
    int rc;

    notif = kmalloc(sizeof(struct nos_notification), GFP_KERNEL);
    if (notif == NULL) {
        return NULL;
    }

    rc = nos_notification_init(notif);
    if (rc != 0) {
        kfree(notif);
        return NULL;
    }

    notif->object.is_static = false;
    nos_object_set_release(&notif->object, nos_notification_free_release);

    if (out_cap != NULL && task != NULL) {
        rc = nos_cap_alloc(&task->cap_table,
                           NOS_CAP_NOTIFICATION,
                           notif,
                           NOS_CAP_RIGHT_ALL,
                           0U,
                           out_cap);
        if (rc < 0) {
            nos_notification_destroy(notif);
            return NULL;
        }
    }

    return notif;
}

int nos_notify_signal(nos_handle_t notif_cap, uint64_t bits)
{
    struct task_struct *task = current;
    struct nos_notification *notif = NULL;
    uint32_t badge = 0U;
    int rc;

    if (task == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_lookup(&task->cap_table,
                        notif_cap,
                        NOS_CAP_NOTIFICATION,
                        NOS_CAP_RIGHT_SEND,
                        (void **)&notif,
                        &badge);
    if (rc < 0) {
        return rc;
    }

    return nos_notify_signal_obj(notif, (badge != 0U) ? (bits | (uint64_t)badge) : bits);
}

int nos_notify_wait(nos_handle_t notif_cap,
                    uint64_t *out_bits,
                    uint32_t timeout_ticks)
{
    struct task_struct *task = current;
    struct nos_notification *notif = NULL;
    nos_timeout_t timeout;
    int rc;

    if (task == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_lookup(&task->cap_table,
                        notif_cap,
                        NOS_CAP_NOTIFICATION,
                        NOS_CAP_RIGHT_RECV,
                        (void **)&notif,
                        NULL);
    if (rc < 0) {
        return rc;
    }

    /* 转换旧版 UAPI 兼容超时常量至统一 nos_timeout_t */
    if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK) {
        timeout = NOS_NO_WAIT;
    } else if (timeout_ticks == NOS_IPC_TIMEOUT_INFINITE) {
        timeout = NOS_WAIT_FOREVER;
    } else {
        timeout = (nos_timeout_t)timeout_ticks;
    }

    return nos_notify_wait_obj(notif, out_bits, timeout);
}
