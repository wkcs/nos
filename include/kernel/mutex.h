/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_MUTEX_H__
#define __NOS_MUTEX_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>

struct mutex {
    bool locked;
    struct list_head wait_list;
    struct task_struct *owner;
    spinlock_t lock;
};

int mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
int mutex_try_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

#endif /* __NOS_MUTEX_H__ */
