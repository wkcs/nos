/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/kernel.h>
#include <kernel/task.h>
#define MUTEX_PI_MAX_DEPTH 4U
struct mutex {
    bool locked;
    struct list_head wait_list;
    struct list_head held_node;
    struct task_struct *owner;
    spinlock_t lock;
};
int mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
int mutex_lock_timeout(struct mutex *lock, uint32_t tick);
int mutex_try_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
bool mutex_is_locked(const struct mutex *lock);
