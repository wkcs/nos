/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NOS 完成量 (nos_completion) 核心实现
 * 支持单次完成 (nos_complete) 与广播完成 (nos_complete_all)。
 */

#define pr_fmt(fmt) "[NOS_COMPLETION]:%s[%d]:" fmt, __func__, __LINE__

#include <nos/completion.h>
#include <nos/wait.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <kernel/errno.h>

int nos_completion_init(struct nos_completion *comp)
{
    int rc;

    if (comp == NULL) {
        return -EINVAL;
    }

    rc = nos_object_init(&comp->object, true);
    if (rc != 0) {
        return rc;
    }

    comp->done = 0U;
    nos_wait_queue_init(&comp->wait_queue);

    return 0;
}

int nos_completion_wait(struct nos_completion *comp, nos_timeout_t timeout)
{
    struct task_struct *task;
    uint64_t deadline;
    int rc;

    if (comp == NULL) {
        return -EINVAL;
    }

    task = current;

    /* 无当前任务上下文（早期启动或非任务环境）仅支持非阻塞判定与已完成快速通道 */
    if (task == NULL) {
        spin_lock_irq(&comp->object.lock);
        if (comp->object.closed) {
            spin_unlock_irq(&comp->object.lock);
            return -EPIPE;
        }
        if (comp->done == UINT32_MAX) {
            spin_unlock_irq(&comp->object.lock);
            return 0;
        }
        if (comp->done > 0U) {
            comp->done--;
            spin_unlock_irq(&comp->object.lock);
            return 0;
        }
        spin_unlock_irq(&comp->object.lock);
        return -EAGAIN;
    }

    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use completion\n");
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&comp->object.lock);

    /* 校验对象是否已关闭 */
    if (comp->object.closed) {
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return -EPIPE;
    }

    /* 快路径 1: 广播完成状态 (UINT32_MAX)，所有等待者均直接穿透放行 */
    if (comp->done == UINT32_MAX) {
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    /* 快路径 2: 单次完成计数大于 0，消耗 1 次完成计数并返回 */
    if (comp->done > 0U) {
        comp->done--;
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    /* 未完成且指定非阻塞时，立即返回 -EAGAIN */
    if (timeout == NOS_NO_WAIT) {
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return -EAGAIN;
    }

    /* 慢路径：准备挂入等待队列阻塞休眠 */
    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }
    rc = nos_wait_prepare(&task->wait_entry, &comp->object, deadline);
    if (rc < 0) {
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    /* 完成量遵循 FIFO 顺序唤醒等待任务 */
    nos_wait_queue_add_tail(&comp->wait_queue, &task->wait_entry);
    task_set_wchan(task, "nos_comp", (uintptr_t)comp);

    rc = task_hang_lock(task);
    if (rc < 0) {
        nos_wait_queue_del(&comp->wait_queue, &task->wait_entry);
        nos_wait_finish(&task->wait_entry);
        task_clear_wchan(task);
        spin_unlock_irq(&comp->object.lock);
        spin_unlock_irq(&task->lock);
        return rc;
    }

    /* 若设置了有限超时，启动任务定时器 */
    if (timeout != NOS_WAIT_FOREVER) {
        timer_start(&task->timer, (uint32_t)timeout);
    }

    spin_unlock_irq(&comp->object.lock);
    spin_unlock_irq(&task->lock);

    /* 切换调度，当前任务让出 CPU 阻塞休眠 */
    switch_task();

    /*
     * 任务被唤醒后（可能由 complete/complete_all/close 唤醒，或由 timer 定时器超时唤醒）：
     * 在对象锁内执行单点状态机终态确认。
     */
    spin_lock_irq(&comp->object.lock);
    if (task->wait_entry.state == NOS_WAIT_STATE_WAITING) {
        /*
         * 若仍处于 WAITING，说明属于定时器超时或外部取消唤醒，
         * 通过 nos_wait_complete_locked 裁决超时终态并从队列摘除。
         */
        (void)nos_wait_complete_locked(&task->wait_entry, -ETIMEDOUT, NULL);
    }
    rc = task->wait_entry.result;
    spin_unlock_irq(&comp->object.lock);

    if (timeout != NOS_WAIT_FOREVER) {
        timer_stop(&task->timer);
    }
    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
    }
    task_clear_wchan(task);

    /* 结束等待，清理节点关联并释放持有的对象引用 */
    nos_wait_finish(&task->wait_entry);

    return rc;
}

int nos_complete(struct nos_completion *comp)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    int rc = 0;

    if (comp == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&comp->object.lock);

    /* 校验对象是否已关闭 */
    if (comp->object.closed) {
        spin_unlock_irq(&comp->object.lock);
        return -EPIPE;
    }

    /* 广播完成状态下，保持完成态不变直接返回 */
    if (comp->done == UINT32_MAX) {
        spin_unlock_irq(&comp->object.lock);
        return 0;
    }

    /* 若等待队列非空，按 FIFO 取出队首等待项予以完成唤醒 */
    if (!nos_wait_queue_empty(&comp->wait_queue)) {
        entry = nos_wait_queue_first(&comp->wait_queue);
        if (entry != NULL) {
            rc = nos_wait_complete_locked(entry, 0, &batch);
            if (rc != 0) {
                nos_wait_queue_del(&comp->wait_queue, entry);
            }
        }
        spin_unlock_irq(&comp->object.lock);

        /* 在对象锁外安全分发唤醒批次，避免反向锁序 */
        nos_wake_batch_dispatch(&batch);
        return 0;
    }

    /* 等待队列为空：若未达保留的广播阈值，累加完成计数 */
    if (comp->done < UINT32_MAX - 1U) {
        comp->done++;
    }

    spin_unlock_irq(&comp->object.lock);
    return 0;
}

int nos_complete_all(struct nos_completion *comp)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    int rc;

    if (comp == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&comp->object.lock);

    /* 校验对象是否已关闭 */
    if (comp->object.closed) {
        spin_unlock_irq(&comp->object.lock);
        return -EPIPE;
    }

    /* 置为广播完成态 UINT32_MAX */
    comp->done = UINT32_MAX;

    /* 唤醒等待队列中的所有活动等待者，并赋成功返回值 0 */
    while (!nos_wait_queue_empty(&comp->wait_queue)) {
        entry = nos_wait_queue_first(&comp->wait_queue);
        if (entry == NULL) {
            break;
        }
        rc = nos_wait_complete_locked(entry, 0, &batch);
        if (rc != 0) {
            nos_wait_queue_del(&comp->wait_queue, entry);
        }
    }

    spin_unlock_irq(&comp->object.lock);

    /* 在对象锁外分发所有唤醒任务 */
    nos_wake_batch_dispatch(&batch);

    return 0;
}

int nos_completion_reset(struct nos_completion *comp)
{
    if (comp == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&comp->object.lock);

    /* 校验对象是否已关闭 */
    if (comp->object.closed) {
        spin_unlock_irq(&comp->object.lock);
        return -EPIPE;
    }

    /* 若仍有等待者正在排队，拒绝重置并返回 -EBUSY */
    if (!nos_wait_queue_empty(&comp->wait_queue)) {
        spin_unlock_irq(&comp->object.lock);
        return -EBUSY;
    }

    /* 清除完成状态 (包括单次计数与 UINT32_MAX 广播完成状态) */
    comp->done = 0U;

    spin_unlock_irq(&comp->object.lock);
    return 0;
}

int nos_completion_close(struct nos_completion *comp)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry;
    int rc;

    if (comp == NULL) {
        return -EINVAL;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&comp->object.lock);

    /* 幂等性：若已关闭直接返回 0 */
    if (comp->object.closed) {
        spin_unlock_irq(&comp->object.lock);
        return 0;
    }

    comp->object.closed = true;

    /* 唤醒当前等待队列中的所有任务并返回 -EPIPE */
    while (!nos_wait_queue_empty(&comp->wait_queue)) {
        entry = nos_wait_queue_first(&comp->wait_queue);
        if (entry == NULL) {
            break;
        }
        rc = nos_wait_complete_locked(entry, -EPIPE, &batch);
        if (rc != 0) {
            nos_wait_queue_del(&comp->wait_queue, entry);
        }
    }

    spin_unlock_irq(&comp->object.lock);

    /* 在对象锁外安全分发唤醒批次 */
    nos_wake_batch_dispatch(&batch);

    return 0;
}
