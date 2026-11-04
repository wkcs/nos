/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 有界定长消息队列 (nos_msgq) 核心实现
 * 严格保证热路径零堆分配、双向背压及 ISR 快速失败。
 */

#define pr_fmt(fmt) "[NOS_MSGQ]:%s[%d]:" fmt, __func__, __LINE__

#include <nos/msgq.h>
#include <nos/wait.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/errno.h>
#include <string.h>

/* 中断上下文探测 (优先架构 HAL 硬件寄存器，回退全局中断嵌套计数) */
extern volatile uint32_t interrupt_nest __attribute__((weak));
extern bool hal_in_irq(void) __attribute__((weak));

static inline bool nos_msgq_in_isr(void)
{
    if (&hal_in_irq != NULL && hal_in_irq()) {
        return true;
    }
    if (&interrupt_nest != NULL && interrupt_nest > 0U) {
        return true;
    }
    return false;
}

int nos_msgq_init(struct nos_msgq *q, void *buffer, size_t buffer_bytes,
                  size_t element_size, uint32_t capacity)
{
    int rc;

    if (q == NULL || buffer == NULL || element_size == 0 || capacity == 0) {
        return -EINVAL;
    }

    /* 缓冲区大小与算术溢出校验: 确保 capacity * element_size 不溢出且 buffer_bytes 充足 */
    if (buffer_bytes / capacity < element_size) {
        return -EINVAL;
    }

    rc = nos_object_init(&q->object, true);
    if (rc != 0) {
        return rc;
    }

    q->buffer = (uint8_t *)buffer;
    q->element_size = element_size;
    q->capacity = capacity;
    q->head = 0U;
    q->tail = 0U;
    q->count = 0U;
    nos_wait_queue_init(&q->wait_send);
    nos_wait_queue_init(&q->wait_recv);

    return 0;
}

int nos_msgq_send(struct nos_msgq *q, const void *element, size_t size,
                  nos_timeout_t timeout)
{
    struct task_struct *task;
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    uint64_t deadline;
    int rc = 0;

    if (q == NULL || element == NULL) {
        return -EINVAL;
    }
    if (size != q->element_size) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);
    task = current;

    /*
     * 非任务上下文 (无 task_struct) 或处于中断 (ISR) 环境：
     * 严禁任何阻塞休眠，必须快速判定返回。
     */
    if (task == NULL || nos_msgq_in_isr()) {
        spin_lock_irq(&q->object.lock);
        if (q->object.closed) {
            spin_unlock_irq(&q->object.lock);
            return -EPIPE;
        }
        if (q->count < q->capacity) {
            /* 队列有空闲插槽，写入环形缓冲并推进尾指针 */
            memcpy(q->buffer + (size_t)q->tail * q->element_size, element, q->element_size);
            q->tail = (q->tail + 1U) % q->capacity;
            q->count++;

            /* 若有等待接收者，唤醒最高优先级接收任务 */
            if (!nos_wait_queue_empty(&q->wait_recv)) {
                entry = nos_wait_queue_first(&q->wait_recv);
                if (entry != NULL) {
                    (void)nos_wait_complete_locked(entry, 0, &batch);
                }
            }
            spin_unlock_irq(&q->object.lock);
            nos_wake_batch_dispatch(&batch);
            return 0;
        }
        spin_unlock_irq(&q->object.lock);
        /* 队列已满：在 ISR 环境下返回 -ENOSPC，无任务非阻塞环境下返回 -EAGAIN */
        return nos_msgq_in_isr() ? -ENOSPC : -EAGAIN;
    }

    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use msgq send\n");
        return -EINVAL;
    }

    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        return rc;
    }

    /* 任务上下文等待循环：处理双向背压与并发竞争 */
    for (;;) {
        spin_lock_irq(&task->lock);
        spin_lock_irq(&q->object.lock);

        if (q->object.closed) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -EPIPE;
        }

        /* 若队列未满，直接写入消息并唤醒等待的接收者 */
        if (q->count < q->capacity) {
            memcpy(q->buffer + (size_t)q->tail * q->element_size, element, q->element_size);
            q->tail = (q->tail + 1U) % q->capacity;
            q->count++;

            if (!nos_wait_queue_empty(&q->wait_recv)) {
                entry = nos_wait_queue_first(&q->wait_recv);
                if (entry != NULL) {
                    (void)nos_wait_complete_locked(entry, 0, &batch);
                }
            }
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            nos_wake_batch_dispatch(&batch);
            return 0;
        }

        /* 队列已满且指定非阻塞，立即返回 -EAGAIN */
        if (timeout == NOS_NO_WAIT) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -EAGAIN;
        }

        /* 检查超时截止期 */
        if (timeout != NOS_WAIT_FOREVER && cpu_run_ticks() >= deadline) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -ETIMEDOUT;
        }

        /* 慢路径：挂入 wait_send 等待队列阻塞休眠 */
        rc = nos_wait_prepare(&task->wait_entry, &q->object, deadline);
        if (rc < 0) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return rc;
        }

        nos_wait_queue_add_prio(&q->wait_send, &task->wait_entry);
        task_set_wchan(task, "nos_msgq_s", (uintptr_t)q);

        rc = task_hang_lock(task);
        if (rc < 0) {
            nos_wait_queue_del(&q->wait_send, &task->wait_entry);
            nos_wait_finish(&task->wait_entry);
            task_clear_wchan(task);
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return rc;
        }

        if (timeout != NOS_WAIT_FOREVER) {
            uint64_t now = cpu_run_ticks();
            uint32_t ticks_left = (now < deadline) ? (uint32_t)(deadline - now) : 1U;
            timer_start(&task->timer, ticks_left);
        }

        spin_unlock_irq(&q->object.lock);
        spin_unlock_irq(&task->lock);

        /* 切换调度，当前任务让出 CPU 阻塞休眠 */
        switch_task();

        /*
         * 任务被唤醒后（可能由 recv 腾出空间唤醒、close 唤醒，或由 timer 定时器超时唤醒）：
         * 在对象锁内执行单点状态机终态裁决。
         */
        spin_lock_irq(&q->object.lock);
        if (task->wait_entry.state == NOS_WAIT_STATE_WAITING) {
            (void)nos_wait_complete_locked(&task->wait_entry, -ETIMEDOUT, NULL);
        }
        rc = task->wait_entry.result;
        spin_unlock_irq(&q->object.lock);

        if (timeout != NOS_WAIT_FOREVER) {
            timer_stop(&task->timer);
        }
        if (task->flag == -ETIMEDOUT) {
            task->flag = 0;
        }
        task_clear_wchan(task);
        nos_wait_finish(&task->wait_entry);

        /* 若超时或被 close 中断，返回错误码；若被成功唤醒 (rc == 0)，循环重新尝试写入 */
        if (rc < 0) {
            return rc;
        }
    }
}

int nos_msgq_recv(struct nos_msgq *q, void *element, size_t size,
                  nos_timeout_t timeout)
{
    struct task_struct *task;
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    uint64_t deadline;
    int rc = 0;

    if (q == NULL || element == NULL) {
        return -EINVAL;
    }
    if (size < q->element_size) {
        return -EMSGSIZE;
    }

    nos_wake_batch_init(&batch);
    task = current;

    /*
     * 非任务上下文 (无 task_struct) 或处于中断 (ISR) 环境：
     * 严禁任何阻塞休眠，必须快速判定返回。
     */
    if (task == NULL || nos_msgq_in_isr()) {
        spin_lock_irq(&q->object.lock);
        if (q->count > 0U) {
            /* 队列中有消息，拷贝出队并推进读指针 */
            memcpy(element, q->buffer + (size_t)q->head * q->element_size, q->element_size);
            q->head = (q->head + 1U) % q->capacity;
            q->count--;

            /* 若有等待发送者，唤醒最高优先级发送任务 */
            if (!nos_wait_queue_empty(&q->wait_send)) {
                entry = nos_wait_queue_first(&q->wait_send);
                if (entry != NULL) {
                    (void)nos_wait_complete_locked(entry, 0, &batch);
                }
            }
            spin_unlock_irq(&q->object.lock);
            nos_wake_batch_dispatch(&batch);
            return 0;
        }

        /* 队列为空 */
        if (q->object.closed) {
            spin_unlock_irq(&q->object.lock);
            return -EPIPE;
        }
        spin_unlock_irq(&q->object.lock);
        return -EAGAIN;
    }

    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use msgq recv\n");
        return -EINVAL;
    }

    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        return rc;
    }

    /* 任务上下文等待循环：处理双向背压与并发竞争 */
    for (;;) {
        spin_lock_irq(&task->lock);
        spin_lock_irq(&q->object.lock);

        /* 若队列中有消息，直接读取并唤醒等待的发送者 */
        if (q->count > 0U) {
            memcpy(element, q->buffer + (size_t)q->head * q->element_size, q->element_size);
            q->head = (q->head + 1U) % q->capacity;
            q->count--;

            if (!nos_wait_queue_empty(&q->wait_send)) {
                entry = nos_wait_queue_first(&q->wait_send);
                if (entry != NULL) {
                    (void)nos_wait_complete_locked(entry, 0, &batch);
                }
            }
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            nos_wake_batch_dispatch(&batch);
            return 0;
        }

        /* 队列为空且已被关闭，返回 -EPIPE */
        if (q->object.closed) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -EPIPE;
        }

        /* 队列为空且指定非阻塞，立即返回 -EAGAIN */
        if (timeout == NOS_NO_WAIT) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -EAGAIN;
        }

        /* 检查超时截止期 */
        if (timeout != NOS_WAIT_FOREVER && cpu_run_ticks() >= deadline) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return -ETIMEDOUT;
        }

        /* 慢路径：挂入 wait_recv 等待队列阻塞休眠 */
        rc = nos_wait_prepare(&task->wait_entry, &q->object, deadline);
        if (rc < 0) {
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return rc;
        }

        nos_wait_queue_add_prio(&q->wait_recv, &task->wait_entry);
        task_set_wchan(task, "nos_msgq_r", (uintptr_t)q);

        rc = task_hang_lock(task);
        if (rc < 0) {
            nos_wait_queue_del(&q->wait_recv, &task->wait_entry);
            nos_wait_finish(&task->wait_entry);
            task_clear_wchan(task);
            spin_unlock_irq(&q->object.lock);
            spin_unlock_irq(&task->lock);
            return rc;
        }

        if (timeout != NOS_WAIT_FOREVER) {
            uint64_t now = cpu_run_ticks();
            uint32_t ticks_left = (now < deadline) ? (uint32_t)(deadline - now) : 1U;
            timer_start(&task->timer, ticks_left);
        }

        spin_unlock_irq(&q->object.lock);
        spin_unlock_irq(&task->lock);

        /* 切换调度，当前任务让出 CPU 阻塞休眠 */
        switch_task();

        /*
         * 任务被唤醒后（可能由 send 送入消息唤醒、close 唤醒，或由 timer 定时器超时唤醒）：
         * 在对象锁内执行单点状态机终态裁决。
         */
        spin_lock_irq(&q->object.lock);
        if (task->wait_entry.state == NOS_WAIT_STATE_WAITING) {
            (void)nos_wait_complete_locked(&task->wait_entry, -ETIMEDOUT, NULL);
        }
        rc = task->wait_entry.result;
        spin_unlock_irq(&q->object.lock);

        if (timeout != NOS_WAIT_FOREVER) {
            timer_stop(&task->timer);
        }
        if (task->flag == -ETIMEDOUT) {
            task->flag = 0;
        }
        task_clear_wchan(task);
        nos_wait_finish(&task->wait_entry);

        /* 若超时或被 close 中断，返回错误码；若被成功唤醒 (rc == 0)，循环重新尝试读取 */
        if (rc < 0) {
            return rc;
        }
    }
}

int nos_msgq_close(struct nos_msgq *q)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;

    if (q == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&q->object.lock);

    /* 幂等性：若已关闭直接返回 0 */
    if (q->object.closed) {
        spin_unlock_irq(&q->object.lock);
        return 0;
    }

    q->object.closed = true;

    /* 唤醒所有等待发送的任务，返回 -EPIPE */
    while (!nos_wait_queue_empty(&q->wait_send)) {
        entry = nos_wait_queue_first(&q->wait_send);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    /* 唤醒所有等待接收的任务，返回 -EPIPE */
    while (!nos_wait_queue_empty(&q->wait_recv)) {
        entry = nos_wait_queue_first(&q->wait_recv);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    spin_unlock_irq(&q->object.lock);

    /* 在对象锁外分发唤醒批次 */
    nos_wake_batch_dispatch(&batch);

    return 0;
}
