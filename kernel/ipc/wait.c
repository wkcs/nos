/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用等待底座 (nos_wait_entry, nos_wait_queue)
 * 与单点仲裁状态机、延后唤醒批次 (nos_wake_batch) 实现。
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <nos/object.h>
#include <nos/wait.h>

/**
 * 弱符号默认实现：持有 task->lock 时置任务为可运行状态
 * 若内核任务模块已链接，由 kernel/task/task.c 覆盖并执行实际调度入队
 */
__attribute__((weak)) void nos_task_wake_up_locked(struct task_struct *task)
{
    if (task != NULL) {
        task->status = 0x02; /* TASK_READY */
    }
}

void nos_wait_entry_init(struct nos_wait_entry *entry, struct task_struct *task)
{
    if (entry == NULL) {
        return;
    }

    INIT_LIST_HEAD(&entry->wait_node);
    INIT_LIST_HEAD(&entry->wake_node);
    entry->epoch = 0ULL;
    entry->state = NOS_WAIT_STATE_IDLE;
    entry->result = 0;
    entry->deadline = 0ULL;
    entry->waiting_obj = NULL;
    entry->queue = NULL;
    entry->task = task;
}

int nos_wait_prepare(struct nos_wait_entry *entry, struct nos_object *obj, uint64_t deadline)
{
    if (entry == NULL || obj == NULL || entry->task == NULL) {
        return -EINVAL;
    }

    /* 同一任务同时只允许一个在途阻塞操作 */
    if (entry->state != NOS_WAIT_STATE_IDLE) {
        return -EBUSY;
    }

    /* 对象关闭后禁止接纳新等待者 */
    if (nos_object_is_closed(obj)) {
        return -EPIPE;
    }

    /* 等待代数严格递增，防回绕 */
    if (entry->epoch == UINT64_MAX) {
        return -EOVERFLOW;
    }

    /* 固定对象引用，确保等待生命周期内对象不被提前析构 */
    if (!nos_object_get(obj)) {
        return -EINVAL;
    }

    entry->epoch++;
    entry->state = NOS_WAIT_STATE_WAITING;
    entry->result = 0;
    entry->deadline = deadline;
    entry->waiting_obj = obj;
    INIT_LIST_HEAD(&entry->wait_node);
    INIT_LIST_HEAD(&entry->wake_node);

    return 0;
}

void nos_wait_finish(struct nos_wait_entry *entry)
{
    if (entry == NULL) {
        return;
    }

    /* 确保从等待链表及唤醒批次链表中安全摘除 */
    if (entry->queue != NULL) {
        nos_wait_queue_del(entry->queue, entry);
        entry->queue = NULL;
    } else if (!list_empty(&entry->wait_node)) {
        list_del_init(&entry->wait_node);
    }
    if (!list_empty(&entry->wake_node)) {
        list_del_init(&entry->wake_node);
    }

    /* 释放先前在 prepare 中固定的对象引用 */
    if (entry->waiting_obj != NULL) {
        nos_object_put(entry->waiting_obj);
        entry->waiting_obj = NULL;
    }

    entry->deadline = 0ULL;
    entry->state = NOS_WAIT_STATE_IDLE;
}

int nos_wait_complete_state_locked(struct nos_wait_entry *entry,
                                   enum nos_wait_state state,
                                   int32_t result,
                                   struct nos_wake_batch *batch)
{
    if (entry == NULL) {
        return -EINVAL;
    }

    /*
     * 单一线性化裁决点：必须且只能在持有 object->lock 的临界区内，
     * 从 WAITING 状态一次性跃迁至目标终态。
     * 若已先一步被其他竞争路径（如资源交付或超时）提交，本次调用作为 no-op 放弃。
     */
    if (entry->state != NOS_WAIT_STATE_WAITING) {
        return -EALREADY;
    }

    entry->state = (uint8_t)state;
    entry->result = result;

    /* 从对象等待队列摘出并同步更新队列计数 */
    if (entry->queue != NULL) {
        nos_wait_queue_del(entry->queue, entry);
        entry->queue = NULL;
    } else if (!list_empty(&entry->wait_node)) {
        list_del_init(&entry->wait_node);
    }

    /* 若提供了唤醒批次，加入延后唤醒链表 */
    if (batch != NULL) {
        nos_wake_batch_add(batch, entry);
    }

    return 0;
}

int nos_wait_complete_locked(struct nos_wait_entry *entry,
                             int32_t result,
                             struct nos_wake_batch *batch)
{
    enum nos_wait_state state;

    if (result >= 0) {
        state = NOS_WAIT_STATE_SUCCEEDED;
    } else if (result == -ETIMEDOUT) {
        state = NOS_WAIT_STATE_TIMEDOUT;
    } else if (result == -EPIPE) {
        state = NOS_WAIT_STATE_CLOSED;
    } else {
        state = NOS_WAIT_STATE_CANCELLED;
    }

    return nos_wait_complete_state_locked(entry, state, result, batch);
}

void nos_wait_queue_add_tail(struct nos_wait_queue *wq, struct nos_wait_entry *entry)
{
    if (wq == NULL || entry == NULL) {
        return;
    }

    if (list_empty(&entry->wait_node)) {
        list_add_tail(&entry->wait_node, &wq->list);
        entry->queue = wq;
        wq->count++;
    }
}

void nos_wait_queue_add_prio(struct nos_wait_queue *wq, struct nos_wait_entry *entry)
{
    struct nos_wait_entry *cur;
    bool inserted = false;

    if (wq == NULL || entry == NULL) {
        return;
    }

    /* 重复入队防护 */
    if (!list_empty(&entry->wait_node)) {
        return;
    }

    /* 按任务实时优先级插入，同优先级遵循到达次序 (FIFO) */
    if (entry->task != NULL) {
        uint8_t prio = entry->task->current_priority;

        list_for_each_entry(cur, &wq->list, wait_node) {
            if (cur->task != NULL && prio < cur->task->current_priority) {
                list_add_tail(&entry->wait_node, &cur->wait_node);
                inserted = true;
                break;
            }
        }
    }

    if (!inserted) {
        list_add_tail(&entry->wait_node, &wq->list);
    }

    entry->queue = wq;
    wq->count++;
}

void nos_wait_queue_del(struct nos_wait_queue *wq, struct nos_wait_entry *entry)
{
    if (wq == NULL || entry == NULL) {
        return;
    }

    if (!list_empty(&entry->wait_node)) {
        list_del_init(&entry->wait_node);
        entry->queue = NULL;
        if (wq->count > 0U) {
            wq->count--;
        }
    }
}

void nos_wake_batch_add(struct nos_wake_batch *batch, struct nos_wait_entry *entry)
{
    if (batch == NULL || entry == NULL) {
        return;
    }

    /* wake_node 独立于 wait_node，仅在批次中串联一次 */
    if (list_empty(&entry->wake_node)) {
        list_add_tail(&entry->wake_node, &batch->list);
        batch->count++;
    }
}

void nos_wake_batch_dispatch(struct nos_wake_batch *batch)
{
    struct nos_wait_entry *entry;
    struct nos_wait_entry *next;

    if (batch == NULL) {
        return;
    }

    /*
     * 关键锁序保证：dispatch 必须且只能在释放 object->lock 之后被调用。
     * 严禁在持有 object->lock 时反向获取 task->lock！
     */
    list_for_each_entry_safe(entry, next, &batch->list, wake_node) {
        struct task_struct *task = entry->task;

        list_del_init(&entry->wake_node);
        if (batch->count > 0U) {
            batch->count--;
        }

        if (task != NULL) {
            spin_lock_irq(&task->lock);
            nos_task_wake_up_locked(task);
            spin_unlock_irq(&task->lock);
        }
    }
}
