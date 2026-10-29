/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/list.h>
#include <kernel/spinlock.h>
struct task_struct { struct list_head list; spinlock_t lock; spinlock_t *list_lock; const char *name; unsigned current_priority, init_priority; };
extern struct task_struct *current;
int task_hang(struct task_struct *task);
int task_hang_lock(struct task_struct *task);
int task_resume(struct task_struct *task);
int task_set_prio(struct task_struct *task, unsigned prio);
