/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_TASK_H__
#define __NOS_TASK_H__

#include <kernel/kernel.h>
#include <kernel/pid.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <asm/task.h>

enum task_status {
    TASK_RUNING = 0x01,
    TASK_READY = 0x02,
    TASK_WAIT = 0x04,
    TASK_SUSPEND = 0x08,
    TASK_CLOSE = 0x10
};

struct task_struct {
    addr_t *sp;

    const char *name;
    pid_t pid;

    addr_t *entry;
    addr_t *parameter;
    addr_t *stack;

    uint32_t  init_tick;
    uint32_t  remaining_tick;

    void (*cleanup)(struct task_struct *task);

    uint8_t init_priority;
    uint8_t current_priority;
    uint8_t status;
    uint32_t stack_size;

#if CONFIG_MAX_PRIORITY > 32
    uint8_t  offset;
    uint8_t  prio_mask;
#endif
    uint32_t offset_mask;

    int flag;
    struct list_head list;
    struct list_head tlist;

    spinlock_t lock;
    spinlock_t *list_lock;
    struct timer timer;

    u64 start_time;
    u32 run_time;
    u32 sys_cycle;
    u32 save_run_time;
    u32 save_sys_cycle;
};

extern struct task_struct *g_current_task;
#define current g_current_task

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t stack_size,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task));
int task_ready(struct task_struct *task);
int task_yield_cpu(void);
void task_del(struct task_struct *task);
int task_hang(struct task_struct *task);
int task_hang_lock(struct task_struct *task);
int task_resume(struct task_struct *task);
int task_sleep(u32 tick);
int task_set_prio(struct task_struct *task, uint8_t prio);
u32 task_get_cpu_usage(struct task_struct *task);
void clean_close_task(void);
void dump_all_task(void);

#define task_list_lock(lock_func, lock) \
do {                                    \
    if ((lock) != NULL) {               \
        lock_func(lock);                \
    }                                   \
} while (0)

#define task_list_unlock(lock_func, lock) \
do {                                      \
    if ((lock) != NULL) {                 \
        lock_func(lock);                  \
    }                                     \
} while (0)

#endif /* __NOS_TASK_H__ */
