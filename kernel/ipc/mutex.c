/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[MUTEX]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/mutex.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <string.h>

int mutex_init(struct mutex *lock)
{
    if (lock == NULL) {
        pr_err("mutex is NULL\n");
        return -EINVAL;
    }

    lock->locked = false;
    lock->owner = NULL;
    INIT_LIST_HEAD(&lock->wait_list);
    INIT_LIST_HEAD(&lock->held_node);
    spin_lock_init(&lock->lock);

    return 0;
}

bool mutex_is_locked(const struct mutex *lock)
{
    return lock != NULL && lock->locked;
}

static void __mutex_lock_locked(struct task_struct *task, struct mutex *lock)
{
    struct task_struct *task_temp;

    if (!list_empty(&task->list)) {
        BUG_ON(true);
        pr_err("%s task list is not empty\n", task->name);
        return;
    }

    list_for_each_entry (task_temp, &lock->wait_list, list) {
        if (task->current_priority < task_temp->current_priority) {
            break;
        }
    }
    list_add_tail(&task->list, &task_temp->list);
    task->list_lock = &lock->lock;
}

static void __mutex_resort_waiter_locked(struct task_struct *task, struct mutex *lock)
{
    struct task_struct *task_temp;

    list_del(&task->list);
    INIT_LIST_HEAD(&task->list);
    list_for_each_entry (task_temp, &lock->wait_list, list) {
        if (task->current_priority < task_temp->current_priority) {
            break;
        }
    }
    list_add_tail(&task->list, &task_temp->list);
}

static void mutex_propagate_pi(struct task_struct *waiter, struct task_struct *owner)
{
    uint32_t depth;
    int rc;

    for (depth = 0U; depth < MUTEX_PI_MAX_DEPTH; depth++) {
        struct mutex *next_lock;
        struct task_struct *next_owner = NULL;

        if (owner == NULL || owner == waiter) {
            break;
        }
        if (waiter->current_priority >= owner->current_priority) {
            break;
        }
        rc = task_pi_boost(owner, waiter->current_priority);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\n",
                   owner->name, waiter->current_priority, rc);
            break;
        }

        spin_lock_irq(&owner->lock);
        next_lock = owner->blocked_on;
        if (next_lock != NULL) {
            spin_lock_irq(&next_lock->lock);
            if (owner->list_lock == &next_lock->lock && !list_empty(&owner->list)) {
                __mutex_resort_waiter_locked(owner, next_lock);
            }
            next_owner = next_lock->owner;
            spin_unlock_irq(&next_lock->lock);
        }
        spin_unlock_irq(&owner->lock);
        owner = next_owner;
    }
}

static void mutex_unboost_chain(struct task_struct *owner)
{
    uint32_t depth;

    for (depth = 0U; depth < MUTEX_PI_MAX_DEPTH && owner != NULL; depth++) {
        struct mutex *next_lock;
        struct task_struct *next_owner = NULL;

        (void)task_pi_unboost(owner, TASK_PRIO_INVALID);
        spin_lock_irq(&owner->lock);
        next_lock = owner->blocked_on;
        if (next_lock != NULL) {
            spin_lock_irq(&next_lock->lock);
            if (owner->list_lock == &next_lock->lock && !list_empty(&owner->list)) {
                __mutex_resort_waiter_locked(owner, next_lock);
            }
            next_owner = next_lock->owner;
            spin_unlock_irq(&next_lock->lock);
        }
        spin_unlock_irq(&owner->lock);
        owner = next_owner;
    }
}

int mutex_lock_timeout(struct mutex *lock, uint32_t tick)
{
    struct task_struct *task;
    struct task_struct *owner;
    int rc;

    if (lock == NULL) {
        return -EINVAL;
    }

    task = current;
    if (task == NULL) {
        spin_lock_irq(&lock->lock);
        if (!lock->locked) {
            lock->locked = true;
            lock->owner = NULL;
            spin_unlock_irq(&lock->lock);
            return 0;
        }
        spin_unlock_irq(&lock->lock);
        return -EBUSY;
    }
    spin_lock_irq(&task->lock);
    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = task;
        if (task->held_mutex_list.next != NULL) {
            list_add_tail(&lock->held_node, &task->held_mutex_list);
        }
        task->blocked_on = NULL;
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }
    if (lock->owner == task) {
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        pr_err("%s recursive mutex deadlock detected\n",
               task->name != NULL ? task->name : "task");
        return -EDEADLK;
    }
    task_set_wchan(task, "mutex", (uintptr_t)lock);
    task->blocked_on = lock;
    task->flag = 0;
    rc = task_hang_lock(task);
    if (rc < 0) {
        task->blocked_on = NULL;
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\n", task->name, rc);
        return rc;
    }
    __mutex_lock_locked(task, lock);
    owner = lock->owner;
    if (tick > 0U) {
        timer_start(&task->timer, tick);
    }
    /* Keep the owner stable until its inherited priority is applied. */
    sch_lock();
    spin_unlock_irq(&lock->lock);
    spin_unlock_irq(&task->lock);
    mutex_propagate_pi(task, owner);
    sch_unlock();
    switch_task();

    spin_lock_irq(&task->lock);
    spin_lock_irq(&lock->lock);
    task->blocked_on = NULL;
    if (lock->owner == task) {
        task->flag = 0;
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }
    if (task->list_lock == &lock->lock && !list_empty(&task->list)) {
        list_del_init(&task->list);
        task->list_lock = NULL;
    }
    owner = lock->owner;
    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
    }
    spin_unlock_irq(&lock->lock);
    spin_unlock_irq(&task->lock);

    if (owner != NULL) {
        sch_lock();
        mutex_unboost_chain(owner);
        sch_unlock();
        switch_task();
    }
    return -ETIMEDOUT;
}

/*给互斥体加锁*/
void mutex_lock(struct mutex *lock)
{
    (void)mutex_lock_timeout(lock, 0U);
}

int mutex_try_lock(struct mutex *lock)
{
    struct task_struct *task = current;

    if (task != NULL) {
        spin_lock_irq(&task->lock);
    }
    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = task;
        if (task != NULL && task->held_mutex_list.next != NULL) {
            list_add_tail(&lock->held_node, &task->held_mutex_list);
        }
        spin_unlock_irq(&lock->lock);
        if (task != NULL) {
            spin_unlock_irq(&task->lock);
        }
        return 0;
    }
    spin_unlock_irq(&lock->lock);
    if (task != NULL) {
        spin_unlock_irq(&task->lock);
    }

    return -EBUSY;
}

void mutex_unlock(struct mutex *lock)
{
    struct task_struct *owner;
    bool need_unboost = false;
    int rc;

    if (lock == NULL) {
        return;
    }

    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        spin_unlock_irq(&lock->lock);
        return;
    }
    if (lock->owner != current) {
        spin_unlock_irq(&lock->lock);
        pr_err("%s task is not mutex owner\n",
               current != NULL ? current->name : "boot");
        return;
    }
    owner = lock->owner;

    if (list_empty(&lock->wait_list)) {
        lock->locked = false;
        lock->owner = NULL;
        if (lock->held_node.next != NULL && !list_empty(&lock->held_node)) {
            list_del_init(&lock->held_node);
        }
        if (owner != NULL &&
            (owner->pi_boost_prio != TASK_PRIO_INVALID ||
             owner->current_priority != owner->init_priority)) {
            need_unboost = true;
        }
        if (!need_unboost) {
            spin_unlock_irq(&lock->lock);
            return;
        }

        sch_lock();
        spin_unlock_irq(&lock->lock);
        rc = task_pi_unboost(owner, TASK_PRIO_INVALID);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\n",
                   owner->name, owner->init_priority, rc);
        }
        sch_unlock();
        switch_task();
        return;
    }

    sch_lock();
    struct task_struct *task_temp;
    task_temp = list_first_entry(&lock->wait_list, struct task_struct, list);
    list_del(&task_temp->list);
    INIT_LIST_HEAD(&task_temp->list);
    task_temp->list_lock = NULL;
    lock->owner = task_temp;
    task_temp->blocked_on = NULL;
    task_temp->flag = 0;
    if (lock->held_node.next != NULL && !list_empty(&lock->held_node)) {
        list_del_init(&lock->held_node);
    }
    if (task_temp->held_mutex_list.next != NULL) {
        list_add_tail(&lock->held_node, &task_temp->held_mutex_list);
    }
    if (!list_empty(&lock->wait_list)) {
        struct task_struct *next_waiter =
            list_first_entry(&lock->wait_list, struct task_struct, list);
        if (next_waiter->current_priority < task_temp->current_priority) {
            (void)task_pi_boost(task_temp, next_waiter->current_priority);
        }
    }
    spin_unlock_irq(&lock->lock);
    if (owner != NULL) {
        rc = task_pi_unboost(owner, TASK_PRIO_INVALID);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\n",
                   owner->name, owner->init_priority, rc);
        }
    }
    task_resume(task_temp);
    sch_unlock();
    switch_task();
}
