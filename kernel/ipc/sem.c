/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[SEM]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/sem.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <string.h>

int sem_init(sem_t *sem, uint8_t num)
{
    if (sem == NULL) {
        pr_err("sem is NULL\n");
        return -EINVAL;
    }

    sem->count = num;
    INIT_LIST_HEAD(&sem->wait_list);
    spin_lock_init(&sem->lock);

    return 0;
}

static void __sem_get_locked(struct task_struct *task, sem_t *sem)
{
    struct task_struct *task_temp;

    if (!list_empty(&task->list)) {
        BUG_ON(true);
        return;
    }

    list_for_each_entry (task_temp, &sem->wait_list, list) {
        if (task->current_priority < task_temp->current_priority) {
            break;
        }
    }
    list_add_tail(&task->list, &task_temp->list);
    task->list_lock = &sem->lock;
}

void sem_get(sem_t *sem)
{
    struct task_struct *task;
    int rc;

    task = current;
    if (task == NULL) {
        spin_lock_irq(&sem->lock);
        if (sem->count > 0) {
            sem->count--;
        }
        spin_unlock_irq(&sem->lock);
        return;
    }
    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use sem\n");
        return;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irq(&sem->lock);
        spin_unlock_irq(&task->lock);
        return;
    }
    rc = task_hang_lock(task);
    if (rc < 0) {
        spin_unlock_irq(&sem->lock);
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\n", task->name, rc);
        return;
    }
    __sem_get_locked(task, sem);
    spin_unlock_irq(&sem->lock);
    spin_unlock_irq(&task->lock);
    switch_task();
}

int sem_get_timeout(sem_t *sem, uint32_t tick)
{
    struct task_struct *task;
    int rc;

    task = current;
    if (task == NULL) {
        spin_lock_irq(&sem->lock);
        if (sem->count > 0) {
            sem->count--;
            spin_unlock_irq(&sem->lock);
            return 0;
        }
        spin_unlock_irq(&sem->lock);
        return -ETIMEDOUT;
    }
    if (task->pid == IDEL_TASK_PID) {
        pr_fatal("idle task can't use sem\n");
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irq(&sem->lock);
        spin_unlock_irq(&task->lock);
        return 0;
    }

    rc = task_hang_lock(task);
    if (rc < 0) {
        spin_unlock_irq(&sem->lock);
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\n", task->name, rc);
        return rc;
    }
    __sem_get_locked(task, sem);
    spin_unlock_irq(&sem->lock);
    if (tick == 0) {
        spin_unlock_irq(&task->lock);
        switch_task();
        return 0;
    }

    timer_start(&task->timer, tick);
    spin_unlock_irq(&task->lock);
    switch_task();

    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
        return -ETIMEDOUT;
    }

    return 0;
}

void sem_send(sem_t *sem, uint8_t num)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(tmp_list);

    if (num == 0) {
        return;
    }

    spin_lock_irq(&sem->lock);
    if (!list_empty(&sem->wait_list)) {
        list_for_each_entry_safe (task, tmp, &sem->wait_list, list) {
            num--;
            list_del(&task->list);
            task->list_lock = NULL;
            list_add(&task->list, &tmp_list);
            if (num == 0) {
                break;
            }
        }
    }
    if ((uint32_t)sem->count + num > 0xFFU) {
        sem->count = 0xFFU;
    } else {
        sem->count += num;
    }
    spin_unlock_irq(&sem->lock);

    if (!list_empty(&tmp_list)) {
        list_for_each_entry_safe (task, tmp, &tmp_list, list) {
            task_resume(task);
        }
    }

    switch_task();
}

void sem_send_all(sem_t *sem)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(tmp_list);

    spin_lock_irq(&sem->lock);
    if (!list_empty(&sem->wait_list)) {
        list_for_each_entry_safe (task, tmp, &sem->wait_list, list) {
            list_del(&task->list);
            task->list_lock = NULL;
            list_add(&task->list, &tmp_list);
        }
    }
    spin_unlock_irq(&sem->lock);

    if (!list_empty(&tmp_list)) {
        list_for_each_entry_safe (task, tmp, &tmp_list, list) {
            task_resume(task);
        }
    }

    switch_task();
}
