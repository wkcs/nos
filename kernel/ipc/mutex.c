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
        pr_err("mutex is NULL\r\n");
        return -EINVAL;
    }

    lock->locked = false;
    lock->owner = NULL;
    INIT_LIST_HEAD(&lock->wait_list);
    spin_lock_init(&lock->lock);

    return 0;
}

static void __mutex_lock_locked(struct task_struct *task, struct mutex *lock)
{
    struct task_struct *task_temp;

    if (!list_empty(&task->list)) {
        BUG_ON(true);
        pr_err("%s task list is not empty\r\n", task->name);
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

/*给互斥体加锁*/
void mutex_lock(struct mutex *lock)
{
    struct task_struct *task;
    struct task_struct *owner;
    int rc;

    task = current;
    spin_lock_irq(&task->lock);
    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = task;
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        return;
    }
    rc = task_hang_lock(task);
    if (rc < 0) {
        spin_unlock_irq(&lock->lock);
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
        return;
    }
    __mutex_lock_locked(task, lock);
    owner = lock->owner;
    /* Keep the owner stable until its inherited priority is applied. */
    sch_lock();
    spin_unlock_irq(&lock->lock);
    spin_unlock_irq(&task->lock);
    if (task->current_priority < owner->current_priority) {
        rc = task_set_prio(owner, task->current_priority);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\r\n",
                   owner->name, task->current_priority, rc);
        }
    }
    sch_unlock();
    switch_task();
}

int mutex_try_lock(struct mutex *lock)
{
    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = current;
        spin_unlock_irq(&lock->lock);
        return 0;
    }
    spin_unlock_irq(&lock->lock);

    return -EBUSY;
}

void mutex_unlock(struct mutex *lock)
{
    struct task_struct *owner;
    int rc;

    spin_lock_irq(&lock->lock);
    if (!lock->locked) {
        spin_unlock_irq(&lock->lock);
        return;
    }
    if (lock->owner != current) {
        spin_unlock_irq(&lock->lock);
        pr_err("%s task is not mutex owner\r\n", current->name);
        return;
    }
    owner = lock->owner;

    sch_lock();
    if (list_empty(&lock->wait_list)) {
        lock->locked = false;
        lock->owner = NULL;
        spin_unlock_irq(&lock->lock);
        rc = task_set_prio(owner, owner->init_priority);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\r\n",
                   owner->name, owner->init_priority, rc);
        }
        sch_unlock();
        return;
    }

    struct task_struct *task_temp;
    task_temp = list_first_entry(&lock->wait_list, struct task_struct, list);
    list_del(&task_temp->list);
    task_temp->list_lock = NULL;
    lock->owner = task_temp;
    spin_unlock_irq(&lock->lock);
    rc = task_set_prio(owner, owner->init_priority);
    if (rc < 0) {
        pr_err("%s task set priority to %u error, rc=%d\r\n",
               owner->name, owner->init_priority, rc);
    }
    task_resume(task_temp);
    sch_unlock();
    switch_task();
}
