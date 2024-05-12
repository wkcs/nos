/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_TIMER_H__
#define __NOS_TIMER_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

struct timer {
    const char *name;
    uint32_t init_tick;
    uint64_t timeout_tick;
    void (*timeout_func)(void *parameter);
    void *parameter;
    struct list_head list;
    spinlock_t lock;
    bool timeout;
};

int timer_init(struct timer *timer, const char *name,
               void (*timeout)(void *parameter), void *parameter);
struct timer *timer_creat(const char *name, void (*timeout)(void *parameter),
                          void *parameter);
int timer_remove(struct timer *timer);
int timer_start(struct timer *timer, u32 tick);
int timer_stop(struct timer *timer);
void timer_check_handle(void);
void dump_timer(void);

#endif /* __NOS_TIMER_H__ */
