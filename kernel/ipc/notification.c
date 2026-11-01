/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Asynchronous Bitmask Notification Object
 * Safe for IRQ bottom-half signaling, timers, and async event multiplexing.
 */

#define pr_fmt(fmt) "[NOTIFY]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/ipc.h>
#include <nos/cap.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/irq.h>

int nos_notification_init(struct nos_notification *notif)
{
    if (notif == NULL) {
        return -EINVAL;
    }

    spin_lock_init(&notif->lock);
    notif->pending_bits = 0ULL;
    INIT_LIST_HEAD(&notif->wait_queue);
    notif->closed = false;

    return 0;
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
    nos_notification_init(notif);

    if (out_cap != NULL && task != NULL) {
        rc = nos_cap_alloc(&task->cap_table,
                           NOS_CAP_NOTIFICATION,
                           notif,
                           NOS_CAP_RIGHT_ALL,
                           0U,
                           out_cap);
        if (rc < 0) {
            kfree(notif);
            return NULL;
        }
    }

    return notif;
}

void nos_notification_destroy(struct nos_notification *notif)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(wake_list);

    if (notif == NULL) {
        return;
    }

    spin_lock_irq(&notif->lock);
    notif->closed = true;
    list_for_each_entry_safe(task, tmp, &notif->wait_queue, list) {
        list_del(&task->list);
        task->list_lock = NULL;
        task->flag = -EPIPE;
        task->ipc_state = NOS_IPC_STATE_NONE;
        list_add_tail(&task->list, &wake_list);
    }
    spin_unlock_irq(&notif->lock);

    list_for_each_entry_safe(task, tmp, &wake_list, list) {
        list_del_init(&task->list);
        task_resume(task);
    }
    kfree(notif);
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    struct task_struct *waiter = NULL;

    if (notif == NULL || bits == 0ULL) {
        return -EINVAL;
    }

    spin_lock_irq(&notif->lock);
    if (notif->closed) {
        spin_unlock_irq(&notif->lock);
        return -EPIPE;
    }

    notif->pending_bits |= bits;
    if (!list_empty(&notif->wait_queue)) {
        waiter = list_first_entry(&notif->wait_queue, struct task_struct, list);
        list_del_init(&waiter->list);
        waiter->list_lock = NULL;
        waiter->ipc_state = NOS_IPC_STATE_NONE;
        waiter->notify_bits = notif->pending_bits;
        notif->pending_bits = 0ULL;
    }
    spin_unlock_irq(&notif->lock);

    if (waiter != NULL) {
        task_resume(waiter);
        switch_task();
    }

    return 0;
}

int nos_notify_wait_obj(struct nos_notification *notif,
                        uint64_t *out_bits,
                        uint32_t timeout_ticks)
{
    struct task_struct *task = current;
    int rc;

    if (notif == NULL || out_bits == NULL || task == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&notif->lock);
    if (notif->closed) {
        spin_unlock_irq(&notif->lock);
        spin_unlock_irq(&task->lock);
        return -EPIPE;
    }

    if (notif->pending_bits != 0ULL) {
        *out_bits = notif->pending_bits;
        notif->pending_bits = 0ULL;
        spin_unlock_irq(&notif->lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK) {
        spin_unlock_irq(&notif->lock);
        spin_unlock_irq(&task->lock);
        return -EAGAIN;
    }

    task_set_wchan(task, "notify", (uintptr_t)notif);
    rc = task_hang_lock(task);
    if (rc < 0) {
        spin_unlock_irq(&notif->lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }
    task->flag = 0;
    task->notify_bits = 0ULL;
    task->ipc_state = NOS_IPC_STATE_NOTIFY_WAIT;
    list_add_tail(&task->list, &notif->wait_queue);
    task->list_lock = &notif->lock;
    if (timeout_ticks != NOS_IPC_TIMEOUT_INFINITE) {
        timer_start(&task->timer, timeout_ticks);
    }
    spin_unlock_irq(&notif->lock);
    spin_unlock_irq(&task->lock);

    switch_task();

    if (task->flag < 0) {
        rc = task->flag;
        task->flag = 0;
        task->ipc_state = NOS_IPC_STATE_NONE;
        return rc;
    }

    *out_bits = task->notify_bits;
    return 0;
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

    return nos_notify_wait_obj(notif, out_bits, timeout_ticks);
}
