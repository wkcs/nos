/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NOS 32 位计数信号量 (nos_sem) 与通用等待底座集成实现。
 */

#define pr_fmt(fmt) "[NOS_SEM]:%s[%d]:" fmt, __func__, __LINE__

#include <nos/sem.h>
#include <nos/wait.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <kernel/errno.h>
#include <string.h>

/* =========================================================================
 * 现代 32 位计数信号量 (nos_sem) 核心实现
 * ========================================================================= */

int nos_sem_init(struct nos_sem *sem, uint32_t initial, uint32_t limit)
{
    int rc;

    if (sem == NULL || limit == 0U || initial > limit) {
        return -EINVAL;
    }

    rc = nos_object_init(&sem->object, true);
    if (rc != 0) {
        return rc;
    }

    sem->count = initial;
    sem->limit = limit;
    nos_wait_queue_init(&sem->wait_queue);

    return 0;
}

int nos_sem_take(struct nos_sem *sem, nos_timeout_t timeout)
{
    struct task_struct *task;
    uint64_t deadline;
    int rc;

    if (sem == NULL) {
        return -EINVAL;
    }

    task = current;

    /* 无当前任务上下文（早期启动或非任务环境）仅支持非阻塞获取 */
    if (task == NULL) {
        spin_lock_irq(&sem->object.lock);
        if (sem->object.closed) {
            spin_unlock_irq(&sem->object.lock);
            return -EPIPE;
        }
        if (sem->count > 0U) {
            sem->count--;
            spin_unlock_irq(&sem->object.lock);
            return 0;
        }
        spin_unlock_irq(&sem->object.lock);
        return -EAGAIN;
    }

    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use sem\n");
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&sem->object.lock);

    /* 校验对象是否已关闭 */
    if (sem->object.closed) {
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return -EPIPE;
    }

    /* 快路径：若有可用令牌，直接扣减并返回 */
    if (sem->count > 0U) {
        sem->count--;
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    /* 令牌不足且指定非阻塞时，立即返回 -EAGAIN */
    if (timeout == NOS_NO_WAIT) {
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return -EAGAIN;
    }

    /* 慢路径：准备挂入等待队列阻塞休眠 */
    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }
    rc = nos_wait_prepare(&task->wait_entry, &sem->object, deadline);
    if (rc < 0) {
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    nos_wait_queue_add_prio(&sem->wait_queue, &task->wait_entry);
    task_set_wchan(task, "nos_sem", (uintptr_t)sem);

    rc = task_hang_lock(task);
    if (rc < 0) {
        nos_wait_queue_del(&sem->wait_queue, &task->wait_entry);
        nos_wait_finish(&task->wait_entry);
        task_clear_wchan(task);
        spin_unlock_irq(&sem->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    /* 若设置了有限超时，启动任务定时器 */
    if (timeout != NOS_WAIT_FOREVER) {
        timer_start(&task->timer, (uint32_t)timeout);
    }

    spin_unlock_irq(&sem->object.lock);
    spin_unlock_irq(&task->lock);

    /* 切换调度，当前任务让出 CPU 阻塞休眠 */
    switch_task();

    /*
     * 任务被唤醒后（可能由 give/close 唤醒，或由 timer 定时器超时唤醒）：
     * 在对象锁内执行单点状态机线性化终态确认。
     */
    spin_lock_irq(&sem->object.lock);
    if (task->wait_entry.state == NOS_WAIT_STATE_WAITING) {
        /*
         * 若仍处于 WAITING，说明属于定时器超时或外部取消唤醒，
         * 通过 nos_wait_complete_locked 裁决超时终态并从队列摘除。
         */
        (void)nos_wait_complete_locked(&task->wait_entry, -ETIMEDOUT, NULL);
    }
    rc = task->wait_entry.result;
    spin_unlock_irq(&sem->object.lock);

    /* 清理可能遗留的旧标志并清理 wchan */
    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
    }
    task_clear_wchan(task);

    /* 结束等待，清理节点关联并释放持有的对象引用 */
    nos_wait_finish(&task->wait_entry);

    return rc;
}

int nos_sem_trytake(struct nos_sem *sem)
{
    return nos_sem_take(sem, NOS_NO_WAIT);
}

int nos_sem_give(struct nos_sem *sem)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    int rc = 0;

    if (sem == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&sem->object.lock);

    /* 校验对象是否已关闭 */
    if (sem->object.closed) {
        spin_unlock_irq(&sem->object.lock);
        return -EPIPE;
    }

    /* 若等待队列非空，直接将令牌交接给最高优先级的等待任务 */
    if (!nos_wait_queue_empty(&sem->wait_queue)) {
        entry = nos_wait_queue_first(&sem->wait_queue);
        if (entry != NULL) {
            /* 单点线性化：完成等待并加入延后唤醒批次，count 保持不变 */
            rc = nos_wait_complete_locked(entry, 0, &batch);
        }
        spin_unlock_irq(&sem->object.lock);

        /* 在对象锁外安全分发唤醒批次，避免反向锁序 */
        nos_wake_batch_dispatch(&batch);
        return rc;
    }

    /* 等待队列为空：若未达上限累加令牌，若已满报错拒绝静默饱和溢出 */
    if (sem->count < sem->limit) {
        sem->count++;
        rc = 0;
    } else {
        rc = -EOVERFLOW;
    }

    spin_unlock_irq(&sem->object.lock);
    return rc;
}

int nos_sem_close(struct nos_sem *sem)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;

    if (sem == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&sem->object.lock);

    /* 幂等性：若已关闭直接返回 0 */
    if (sem->object.closed) {
        spin_unlock_irq(&sem->object.lock);
        return 0;
    }

    sem->object.closed = true;

    /* 唤醒等待队列中的所有等待者，并赋返回值 -EPIPE */
    while (!nos_wait_queue_empty(&sem->wait_queue)) {
        entry = nos_wait_queue_first(&sem->wait_queue);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    spin_unlock_irq(&sem->object.lock);

    /* 在对象锁外分发唤醒批次 */
    nos_wake_batch_dispatch(&batch);

    return 0;
}

