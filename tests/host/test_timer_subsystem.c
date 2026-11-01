/**
 * Copyright (C) 2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/kernel.h>
#include <kernel/timer.h>
#include <kernel/sched.h>

uint32_t scheduler_lock_nest = 0U;
u32 g_sys_cycle = 0U;
u64 sys_heartbeat_time = 0ULL;

static u64 g_mock_ticks = 0ULL;
static u64 g_mock_time_us = 0ULL;
static uint32_t g_switch_task_calls = 0U;
static char g_dump_buf[4096];
static size_t g_dump_pos = 0U;

u64 cpu_run_ticks(void)
{
    return g_mock_ticks;
}

u64 cpu_run_time_us(void)
{
    return g_mock_time_us;
}

uint32_t hal_cpu_id(void)
{
    return 0U;
}

void switch_task(void)
{
    g_switch_task_calls++;
}

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1U, size);
}

void kfree(void *addr)
{
    free(addr);
}

addr_t disable_irq_save(void)
{
    return 0U;
}

void enable_irq_save(addr_t level)
{
    (void)level;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    va_list args;
    int n = 0;
    (void)no_tag;
    (void)level;
    va_start(args, fmt);
    if (g_dump_pos < sizeof(g_dump_buf)) {
        n = vsnprintf(g_dump_buf + g_dump_pos,
                      sizeof(g_dump_buf) - g_dump_pos,
                      fmt,
                      args);
        if (n > 0) {
            g_dump_pos += (size_t)n;
        }
    }
    va_end(args);
    return n;
}

static int capture_dump_print(const char *fmt, ...)
{
    va_list args;
    int n = 0;

    va_start(args, fmt);
    if (g_dump_pos < sizeof(g_dump_buf)) {
        n = vsnprintf(g_dump_buf + g_dump_pos,
                      sizeof(g_dump_buf) - g_dump_pos,
                      fmt,
                      args);
        if (n > 0) {
            g_dump_pos += (size_t)n;
        }
    }
    va_end(args);
    return n;
}

static void advance_tick(void)
{
    g_mock_ticks++;
    g_mock_time_us = g_mock_ticks * (u64)CONFIG_SYS_TICK_MS * 1000ULL;
    sys_heartbeat_time = g_mock_time_us;
    timer_check_handle();
}

struct callback_ctx {
    struct timer *self;
    struct timer *peer;
    int count;
    int restart_ticks;
    bool stop_peer;
    bool list_was_clean_in_cb;
};

static void reentrant_cb(void *param)
{
    struct callback_ctx *ctx = (struct callback_ctx *)param;

    ctx->count++;
    if (ctx->self != NULL) {
        /* One-shot timer must already be detached from any stack tmp_list */
        ctx->list_was_clean_in_cb = (list_empty(&ctx->self->list) != 0) &&
                                    !timer_pending(ctx->self);
        if (ctx->restart_ticks > 0) {
            int ticks = ctx->restart_ticks;
            ctx->restart_ticks = 0;
            (void)timer_start(ctx->self, (u32)ticks);
        }
    }
    if (ctx->stop_peer && ctx->peer != NULL) {
        (void)timer_stop(ctx->peer);
    }
}

/* 1. Verify no dangling stack pointer and safe callback re-entrancy */
static void test_callback_reentrancy_and_stack_safety(void)
{
    struct timer t1;
    struct timer t2;
    struct callback_ctx ctx1;
    struct callback_ctx ctx2;

    g_mock_ticks = 100ULL;
    g_mock_time_us = g_mock_ticks * 1000ULL;
    sys_heartbeat_time = g_mock_time_us;

    memset(&ctx1, 0, sizeof(ctx1));
    memset(&ctx2, 0, sizeof(ctx2));
    ctx1.self = &t1;
    ctx1.peer = &t2;
    ctx1.restart_ticks = 3;
    ctx1.stop_peer = true;
    ctx2.self = &t2;

    assert(timer_init(&t1, "t1_reent", reentrant_cb, &ctx1) == 0);
    assert(timer_init(&t2, "t2_peer", reentrant_cb, &ctx2) == 0);
    assert(!timer_pending(&t1));

    /* Both expire on the same tick; t1 callback restarts t1 and cancels t2 */
    assert(timer_start(&t1, 2U) == 0);
    assert(timer_start(&t2, 2U) == 0);
    assert(timer_pending(&t1));
    assert(timer_pending(&t2));

    while (ctx1.count == 0) {
        advance_tick();
    }
    assert(ctx1.count == 1);
    assert(ctx1.list_was_clean_in_cb);
    /* t2 was stopped by t1's callback before t2 could fire */
    assert(ctx2.count == 0);
    assert(!timer_pending(&t2));
    assert(list_empty(&t2.list));

    /* t1 was restarted inside its own callback and should fire a second time */
    assert(timer_pending(&t1));
    while (ctx1.count == 1) {
        advance_tick();
    }
    assert(ctx1.count == 2);
    assert(!timer_pending(&t1));
    /* After expiry, t1.list must point to itself, NOT to a destroyed stack frame */
    assert(t1.list.next == &t1.list && t1.list.prev == &t1.list);
}

struct order_log {
    int ids[16];
    int pos;
};

struct order_item {
    struct order_log *log;
    int id;
    struct timer timer;
};

static void order_cb(void *param)
{
    struct order_item *item = (struct order_item *)param;
    item->log->ids[item->log->pos++] = item->id;
}

/* 2. Verify out-of-order insertion, wheel/list ordering, and next_expires */
static void test_ordering_and_next_expires(void)
{
    static const u32 delays[6] = { 25U, 3U, 70U, 1U, 15U, 9U };
    static const int expected[6] = { 3, 1, 5, 4, 0, 2 };
    struct order_log log;
    struct order_item items[6];
    int i;

    memset(&log, 0, sizeof(log));
    for (i = 0; i < 6; i++) {
        items[i].log = &log;
        items[i].id = i;
        assert(timer_init(&items[i].timer, "ord", order_cb, &items[i]) == 0);
        assert(timer_start(&items[i].timer, delays[i]) == 0);
    }

    /* Tickless query must report minimum remaining ticks (> 0 and <= 2) */
    u64 next_wait = timer_next_timeout_ticks();
    assert(next_wait >= 1ULL && next_wait <= 2ULL);

    for (i = 0; i < 80; i++) {
        advance_tick();
    }
    assert(log.pos == 6);
    for (i = 0; i < 6; i++) {
        assert(log.ids[i] == expected[i]);
    }
    assert(timer_next_timeout_ticks() == U64_MAX);
}

static void periodic_cb(void *param)
{
    int *cnt = (int *)param;
    (*cnt)++;
}

/* 3. Verify native periodic timers and timer_mod / timer_remaining_ticks */
static void test_periodic_and_mod(void)
{
    struct timer ptimer;
    struct timer mtimer;
    int pcount = 0;
    int mcount = 0;
    int i;

    assert(timer_init_ex(&ptimer, "periodic", periodic_cb, &pcount,
                         TIMER_FLAG_PERIODIC) == 0);
    assert(timer_start_periodic(&ptimer, 4U) == 0);
    assert(timer_pending(&ptimer));

    assert(timer_init(&mtimer, "mod_timer", periodic_cb, &mcount) == 0);
    assert(timer_start(&mtimer, 5U) == 0);
    assert(timer_remaining_ticks(&mtimer) >= 5U);

    /* Extend mtimer deadline via timer_mod before it expires */
    advance_tick();
    advance_tick();
    assert(mcount == 0);
    assert(timer_mod(&mtimer, 10U) == 0);
    assert(timer_remaining_ticks(&mtimer) >= 10U);

    for (i = 0; i < 18; i++) {
        advance_tick();
    }
    /* Periodic timer (period=4) over 20 ticks should have fired at least 4 times */
    assert(pcount >= 4);
    assert(mcount == 1);
    assert(!timer_pending(&mtimer));

    assert(timer_stop(&ptimer) == 0);
    assert(!timer_pending(&ptimer));
    int saved = pcount;
    for (i = 0; i < 10; i++) {
        advance_tick();
    }
    assert(pcount == saved);
}

/* 4. Verify scheduler_lock_nest deferral and dump_timer_to */
static void test_scheduler_lock_deferral_and_dump(void)
{
    struct timer dtimer;
    int fired = 0;

    assert(timer_init(&dtimer, "defer_tmr", periodic_cb, &fired) == 0);
    assert(timer_start(&dtimer, 2U) == 0);

    g_dump_pos = 0U;
    g_dump_buf[0] = '\0';
    dump_timer_to(capture_dump_print);
    assert(strstr(g_dump_buf, "defer_tmr") != NULL);

    scheduler_lock_nest = 1U;
    advance_tick();
    advance_tick();
    advance_tick();
    /* While scheduler is locked, callback must not fire yet */
    assert(fired == 0);

    scheduler_lock_nest = 0U;
    advance_tick();
    assert(fired == 1);
    assert(!timer_pending(&dtimer));
}

int main(void)
{
    test_callback_reentrancy_and_stack_safety();
    test_ordering_and_next_expires();
    test_periodic_and_mod();
    test_scheduler_lock_deferral_and_dump();
    printf("timer subsystem checks passed\n");
    return 0;
}
