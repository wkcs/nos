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
#include <kernel/sch.h>
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

static void __mutex_lock(struct task_struct *task, struct mutex *lock)
{
    struct task_struct *task_temp;
    int rc;

    if (!list_empty(&task->list)) {
        BUG_ON(true);
        pr_err("%s task list is not empty\r\n", task->name);
        return;
    }

    spin_lock(&lock->lock);
    list_for_each_entry (task_temp, &lock->wait_list, list) {
        if (task->current_priority < task_temp->current_priority) {
            break;
        }
    }
    list_add_tail(&task->list, &task_temp->list);
    task->list_lock = &lock->lock;
    spin_unlock(&lock->lock);

    if (task->current_priority < lock->owner->current_priority) {
        rc = task_set_prio(lock->owner, task->current_priority);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\r\n",
                   lock->owner->name, task->current_priority, rc);
        }
    }
}

/*给互斥体加锁*/
void mutex_lock(struct mutex *lock)
{
    struct task_struct *task;
    int rc;

    task = current;
    spin_lock(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = task;
        spin_unlock(&lock->lock);
        return;
    }
    spin_unlock(&lock->lock);

    rc = task_hang(task);
    if (rc < 0) {
        pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
        return;
    }
    __mutex_lock(task, lock);
    switch_task();
}

int mutex_try_lock(struct mutex *lock)
{
    spin_lock(&lock->lock);
    if (!lock->locked) {
        lock->locked = true;
        lock->owner = current;
        spin_unlock(&lock->lock);
        return 0;
    }
    spin_unlock(&lock->lock);

    return -EBUSY;
}

void mutex_unlock(struct mutex *lock)
{
    struct task_struct *owner;
    int rc;

    if (!lock->locked) {
        return;
    }
    if (lock->owner != current) {
        pr_err("%s task is not mutex owner\r\n", current->name);
        return;
    }
    owner = lock->owner;

    spin_lock(&lock->lock);
    if (list_empty(&lock->wait_list)) {
        lock->locked = false;
        lock->owner = NULL;
        spin_unlock(&lock->lock);
        rc = task_set_prio(owner, owner->init_priority);
        if (rc < 0) {
            pr_err("%s task set priority to %u error, rc=%d\r\n",
                   owner->name, owner->init_priority, rc);
        }
        return;
    }

    struct task_struct *task_temp;
    task_temp = list_first_entry(&lock->wait_list, struct task_struct, list);
    list_del(&task_temp->list);
    task_temp->list_lock = NULL;
    lock->owner = task_temp;
    spin_unlock(&lock->lock);
    rc = task_set_prio(owner, owner->init_priority);
    if (rc < 0) {
        pr_err("%s task set priority to %u error, rc=%d\r\n",
               owner->name, owner->init_priority, rc);
    }
    task_resume(task_temp);
    switch_task();
}
