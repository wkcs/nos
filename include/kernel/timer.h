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

#define TIMER_FLAG_ONE_SHOT  0x0000U
#define TIMER_FLAG_PERIODIC  0x0001U
#define TIMER_FLAG_RUNNING   0x0002U

struct timer {
    const char *name;
    uint32_t init_tick;
    uint64_t timeout_tick;
    void (*timeout_func)(void *parameter);
    void *parameter;
    struct list_head list;
    uint16_t flags;
    uint8_t cpu;
    bool timeout;
};

int timer_init(struct timer *timer, const char *name,
               void (*timeout)(void *parameter), void *parameter);
int timer_init_ex(struct timer *timer, const char *name,
                  void (*timeout)(void *parameter), void *parameter,
                  uint16_t flags);
struct timer *timer_creat(const char *name, void (*timeout)(void *parameter),
                          void *parameter);
int timer_remove(struct timer *timer);
int timer_start(struct timer *timer, u32 tick);
int timer_start_periodic(struct timer *timer, u32 period_tick);
int timer_mod(struct timer *timer, u32 tick);
int timer_stop(struct timer *timer);
bool timer_pending(const struct timer *timer);
u32 timer_remaining_ticks(const struct timer *timer);
u64 timer_next_timeout_ticks(void);
void timer_check_handle(void);
void dump_timer_to(int (*print_fn)(const char *fmt, ...));
void dump_timer(void);

#endif /* __NOS_TIMER_H__ */
