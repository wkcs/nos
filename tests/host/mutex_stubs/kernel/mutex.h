/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/kernel.h>
#include <kernel/task.h>
struct mutex { bool locked; struct list_head wait_list; struct task_struct *owner; spinlock_t lock; };
int mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
int mutex_try_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
