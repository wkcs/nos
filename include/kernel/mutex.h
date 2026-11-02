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

#define MUTEX_PI_MAX_DEPTH 4U

struct mutex {
    bool locked;
    struct list_head wait_list;
    struct list_head held_node;
    struct task_struct *owner;
    spinlock_t lock;
};

#define MUTEX(name) \
struct mutex name = { \
    .locked = false, \
    .wait_list = LIST_HEAD_INIT((name).wait_list), \
    .held_node = LIST_HEAD_INIT((name).held_node), \
    .owner = NULL, \
}

int mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
int mutex_lock_timeout(struct mutex *lock, uint32_t tick);
int mutex_try_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
bool mutex_is_locked(const struct mutex *lock);

#endif /* __NOS_MUTEX_H__ */
