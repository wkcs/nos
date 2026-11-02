/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#define __NOS_TASK_H__
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <stdint.h>
#define TASK_PRIO_INVALID 0xFFU
struct mutex;
struct task_struct {
    struct list_head list;
    struct list_head held_mutex_list;
    struct mutex *blocked_on;
    spinlock_t lock;
    spinlock_t *list_lock;
    struct timer timer;
    const char *name;
    const char *wchan_name;
    uintptr_t wchan_addr;
    uint8_t current_priority;
    uint8_t init_priority;
    uint8_t pi_boost_prio;
    int flag;
};
extern struct task_struct *current;
int task_hang(struct task_struct *task);
int task_hang_lock(struct task_struct *task);
int task_resume(struct task_struct *task);
int task_set_prio(struct task_struct *task, unsigned prio);
int task_pi_boost(struct task_struct *owner, uint8_t waiter_prio);
int task_pi_unboost(struct task_struct *owner, uint8_t next_waiter_prio);
static inline void task_set_wchan(struct task_struct *task, const char *name, uintptr_t addr)
{
    if (task) {
        task->wchan_name = name;
        task->wchan_addr = addr;
    }
}
