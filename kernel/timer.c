/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[TIMER]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/timer.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>
#include <stdarg.h>

#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

#ifndef CONFIG_SMALL_MEMORY
#if CONFIG_NR_CPUS > 1
#define TIMER_WHEEL_BITS    6U
#else
#define TIMER_WHEEL_BITS    3U
#endif
#define TIMER_WHEEL_BUCKETS (1U << TIMER_WHEEL_BITS)
#define TIMER_WHEEL_MASK    (TIMER_WHEEL_BUCKETS - 1U)
#endif

struct timer_base {
    spinlock_t lock;
    uint64_t next_expires;
    uint32_t active_count;
    uint64_t expired_count;
    bool initialized;
#ifdef CONFIG_SMALL_MEMORY
    struct list_head active_list;
#else
    uint64_t bucket_bitmap;
    struct list_head buckets[TIMER_WHEEL_BUCKETS];
#endif
};

static struct timer_base g_timer_bases[CONFIG_NR_CPUS];

static inline uint32_t timer_current_cpu(void)
{
#if CONFIG_NR_CPUS > 1
    uint32_t cpu = hal_cpu_id();
    return (cpu < (uint32_t)CONFIG_NR_CPUS) ? cpu : 0U;
#else
    return 0U;
#endif
}

static struct timer_base *timer_get_base(uint32_t cpu_id)
{
    struct timer_base *base;

    if (cpu_id >= (uint32_t)CONFIG_NR_CPUS) {
        cpu_id = 0U;
    }
    base = &g_timer_bases[cpu_id];
    if (unlikely(!READ_ONCE(base->initialized))) {
        addr_t irq_flags = disable_irq_save();
        if (!base->initialized) {
            spin_lock_init(&base->lock);
            base->next_expires = U64_MAX;
            base->active_count = 0U;
            base->expired_count = 0ULL;
#ifdef CONFIG_SMALL_MEMORY
            INIT_LIST_HEAD(&base->active_list);
#else
            uint32_t i;
            base->bucket_bitmap = 0ULL;
            for (i = 0U; i < TIMER_WHEEL_BUCKETS; i++) {
                INIT_LIST_HEAD(&base->buckets[i]);
            }
#endif
            WRITE_ONCE(base->initialized, true);
        }
        enable_irq_save(irq_flags);
    }
    return base;
}

static void timer_insert_sorted(struct list_head *head, struct timer *timer)
{
    struct timer *pos;

    list_for_each_entry(pos, head, list) {
        if (pos->timeout_tick > timer->timeout_tick) {
            break;
        }
    }
    list_add_tail(&timer->list, &pos->list);
}

static void timer_refresh_next_expires_locked(struct timer_base *base)
{
    if (base->active_count == 0U) {
        WRITE_ONCE(base->next_expires, U64_MAX);
        return;
    }
#ifdef CONFIG_SMALL_MEMORY
    if (!list_empty(&base->active_list)) {
        struct timer *first = list_first_entry(&base->active_list, struct timer, list);
        WRITE_ONCE(base->next_expires, first->timeout_tick);
    } else {
        WRITE_ONCE(base->next_expires, U64_MAX);
    }
#else
    uint64_t map = base->bucket_bitmap;
    uint64_t min_exp = U64_MAX;

    while (map != 0ULL) {
        uint32_t bit = (uint32_t)__builtin_ctzll(map);
        struct list_head *bhead = &base->buckets[bit];
        if (!list_empty(bhead)) {
            struct timer *first = list_first_entry(bhead, struct timer, list);
            if (first->timeout_tick < min_exp) {
                min_exp = first->timeout_tick;
            }
        }
        map &= (map - 1ULL);
    }
    WRITE_ONCE(base->next_expires, min_exp);
#endif
}

static void timer_enqueue_locked(struct timer_base *base, struct timer *timer)
{
#ifdef CONFIG_SMALL_MEMORY
    timer_insert_sorted(&base->active_list, timer);
#else
    uint32_t bucket = (uint32_t)(timer->timeout_tick & TIMER_WHEEL_MASK);
    timer_insert_sorted(&base->buckets[bucket], timer);
    base->bucket_bitmap |= (1ULL << bucket);
#endif
    timer->flags |= TIMER_FLAG_RUNNING;
    timer->timeout = false;
    base->active_count++;
    if (timer->timeout_tick < base->next_expires) {
        WRITE_ONCE(base->next_expires, timer->timeout_tick);
    }
}

static void timer_dequeue_locked(struct timer_base *base, struct timer *timer)
{
    if ((timer->flags & TIMER_FLAG_RUNNING) == 0U || list_empty(&timer->list)) {
        INIT_LIST_HEAD(&timer->list);
        timer->flags &= (uint16_t)~TIMER_FLAG_RUNNING;
        return;
    }

#ifndef CONFIG_SMALL_MEMORY
    uint32_t bucket = (uint32_t)(timer->timeout_tick & TIMER_WHEEL_MASK);
#endif
    list_del_init(&timer->list);
    timer->flags &= (uint16_t)~TIMER_FLAG_RUNNING;
    if (base->active_count > 0U) {
        base->active_count--;
    }
#ifndef CONFIG_SMALL_MEMORY
    if (list_empty(&base->buckets[bucket])) {
        base->bucket_bitmap &= ~(1ULL << bucket);
    }
#endif
    if (base->active_count == 0U || timer->timeout_tick <= base->next_expires) {
        timer_refresh_next_expires_locked(base);
    }
}

int timer_init_ex(struct timer *timer, const char *name,
                  void (*timeout)(void *parameter), void *parameter,
                  uint16_t flags)
{
    if (timer == NULL) {
        pr_err("timer is NULL\n");
        return -EINVAL;
    }

    (void)timer_get_base(0U);
    INIT_LIST_HEAD(&timer->list);
    timer->name = name;
    timer->timeout_func = timeout;
    timer->parameter = parameter;
    timer->init_tick = 0U;
    timer->timeout_tick = 0ULL;
    timer->flags = (uint16_t)(flags & TIMER_FLAG_PERIODIC);
    timer->cpu = 0U;
    timer->timeout = false;

    return 0;
}

int timer_init(struct timer *timer, const char *name,
               void (*timeout)(void *parameter), void *parameter)
{
    return timer_init_ex(timer, name, timeout, parameter, TIMER_FLAG_ONE_SHOT);
}

struct timer *timer_creat(const char *name, void (*timeout)(void *parameter),
                          void *parameter)
{
    struct timer *timer;
    int rc;

    timer = kmalloc(sizeof(struct timer), GFP_KERNEL);
    if (timer == NULL) {
        pr_err("alloc %s timer buf error\n", name);
        return NULL;
    }
    rc = timer_init(timer, name, timeout, parameter);
    if (rc < 0) {
        pr_err("%s timer init error, rc=%d\n", name, rc);
        kfree(timer);
        return NULL;
    }

    return timer;
}

int timer_remove(struct timer *timer)
{
    struct timer_base *base;

    if (timer == NULL) {
        pr_err("timer is NULL\n");
        return -EINVAL;
    }

    base = timer_get_base(timer->cpu);
    spin_lock_irq(&base->lock);
    timer_dequeue_locked(base, timer);
    spin_unlock_irq(&base->lock);

    return 0;
}

u32 timer_remaining_ticks(const struct timer *timer)
{
    u64 now_tick;
    u64 exp_tick;
    u64 diff;

    if (timer == NULL || !timer_pending(timer)) {
        return 0U;
    }
    now_tick = cpu_run_ticks();
    exp_tick = READ_ONCE(timer->timeout_tick);
    if (exp_tick <= now_tick) {
        return 0U;
    }
    diff = exp_tick - now_tick;
    return (diff > (u64)U32_MAX) ? U32_MAX : (u32)diff;
}

bool timer_pending(const struct timer *timer)
{
    if (timer == NULL) {
        return false;
    }
    return (READ_ONCE(timer->flags) & TIMER_FLAG_RUNNING) != 0U;
}

static int timer_arm_internal(struct timer *timer, u32 tick, bool force_periodic)
{
    struct timer_base *old_base;
    struct timer_base *base;
    uint32_t target_cpu;
    u64 now_tick;
    u64 run_times;
    u32 eff_tick;

    if (timer == NULL) {
        pr_err("timer is NULL\n");
        return -EINVAL;
    }
    if (tick == 0U) {
        pr_err("%s timer set tick to 0\n", timer->name ? timer->name : "?");
        return -EINVAL;
    }

    target_cpu = timer_current_cpu();
    old_base = timer_get_base(timer->cpu);
    base = timer_get_base(target_cpu);

    if (old_base != base) {
        spin_lock_irq(&old_base->lock);
        timer_dequeue_locked(old_base, timer);
        spin_unlock_irq(&old_base->lock);
    }

    spin_lock_irq(&base->lock);
    timer_dequeue_locked(base, timer);

    if (force_periodic) {
        timer->flags |= TIMER_FLAG_PERIODIC;
    }
    timer->cpu = (uint8_t)target_cpu;
    now_tick = cpu_run_ticks();
    run_times = cpu_run_time_us();

    if ((timer->flags & TIMER_FLAG_PERIODIC) != 0U) {
        timer->init_tick = tick;
        eff_tick = tick;
    } else {
        if (run_times >= sys_heartbeat_time &&
            (run_times - sys_heartbeat_time) > ((u64)CONFIG_SYS_TICK_MS * 500ULL)) {
            eff_tick = tick;
        } else {
            eff_tick = (tick < U32_MAX) ? (tick + 1U) : U32_MAX;
        }
        timer->init_tick = eff_tick;
    }

    if (U64_MAX - now_tick >= (u64)eff_tick) {
        timer->timeout_tick = now_tick + (u64)eff_tick;
    } else {
        timer->timeout_tick = U64_MAX;
    }

    timer_enqueue_locked(base, timer);
    spin_unlock_irq(&base->lock);
    return 0;
}

int timer_start(struct timer *timer, u32 tick)
{
    return timer_arm_internal(timer, tick, false);
}

int timer_start_periodic(struct timer *timer, u32 period_tick)
{
    return timer_arm_internal(timer, period_tick, true);
}

int timer_mod(struct timer *timer, u32 tick)
{
    return timer_arm_internal(timer, tick, false);
}

int timer_stop(struct timer *timer)
{
    if (timer == NULL) {
        pr_err("timer is NULL\n");
        return -EINVAL;
    }
    return timer_remove(timer);
}

u64 timer_next_timeout_ticks(void)
{
    struct timer_base *base = timer_get_base(timer_current_cpu());
    u64 next_exp = READ_ONCE(base->next_expires);
    u64 now_tick;

    if (next_exp == U64_MAX) {
        return U64_MAX;
    }
    now_tick = cpu_run_ticks();
    return (next_exp > now_tick) ? (next_exp - now_tick) : 0ULL;
}

static struct timer *timer_pick_expired_locked(struct timer_base *base, u64 now_tick)
{
    if (base->active_count == 0U || base->next_expires > now_tick) {
        return NULL;
    }
#ifdef CONFIG_SMALL_MEMORY
    if (!list_empty(&base->active_list)) {
        struct timer *first = list_first_entry(&base->active_list, struct timer, list);
        if (first->timeout_tick <= now_tick) {
            return first;
        }
    }
    return NULL;
#else
    uint64_t map = base->bucket_bitmap;
    struct timer *best = NULL;

    while (map != 0ULL) {
        uint32_t bit = (uint32_t)__builtin_ctzll(map);
        struct list_head *bhead = &base->buckets[bit];
        if (!list_empty(bhead)) {
            struct timer *first = list_first_entry(bhead, struct timer, list);
            if (first->timeout_tick <= now_tick) {
                if (best == NULL || first->timeout_tick < best->timeout_tick) {
                    best = first;
                }
            }
        }
        map &= (map - 1ULL);
    }
    return best;
#endif
}

void timer_check_handle(void)
{
    struct timer_base *base = timer_get_base(timer_current_cpu());
    u64 now_tick = cpu_run_ticks();
    uint32_t fired = 0U;

    /* Lockless fast path: skip IRQ lock when no timer has reached its deadline */
    if (likely(now_tick < READ_ONCE(base->next_expires))) {
        return;
    }
    if (unlikely(READ_ONCE(scheduler_lock_nest) != 0U)) {
        return;
    }

    for (;;) {
        struct timer *timer;
        void (*fn)(void *parameter);
        void *param;

        if (unlikely(READ_ONCE(scheduler_lock_nest) != 0U)) {
            break;
        }

        spin_lock_irq(&base->lock);
        timer = timer_pick_expired_locked(base, now_tick);
        if (timer == NULL) {
            spin_unlock_irq(&base->lock);
            break;
        }

        fn = timer->timeout_func;
        param = timer->parameter;
        timer_dequeue_locked(base, timer);
        timer->timeout = true;
        base->expired_count++;

        if ((timer->flags & TIMER_FLAG_PERIODIC) != 0U && timer->init_tick > 0U) {
            u64 next_exp = timer->timeout_tick + (u64)timer->init_tick;
            if (next_exp <= now_tick) {
                next_exp = now_tick + (u64)timer->init_tick;
            }
            timer->timeout_tick = next_exp;
            timer_enqueue_locked(base, timer);
        }
        spin_unlock_irq(&base->lock);

        if (fn != NULL) {
            fn(param);
        }
        fired++;
    }

    if (fired > 0U) {
        switch_task();
    }
}

void dump_timer_to(int (*print_fn)(const char *fmt, ...))
{
    uint32_t cpu;

    for (cpu = 0U; cpu < (uint32_t)CONFIG_NR_CPUS; cpu++) {
        struct timer_base *base = timer_get_base(cpu);
        struct timer *timer;

        spin_lock_irq(&base->lock);
        if (print_fn != NULL) {
            print_fn("CPU%u timers: active=%u expired=%llu next_expires=%llu\n",
                     (unsigned int)cpu,
                     (unsigned int)base->active_count,
                     (unsigned long long)base->expired_count,
                     (unsigned long long)base->next_expires);
        } else {
            pr_info("CPU%u timers: active=%u expired=%llu next_expires=%llu\n",
                    (unsigned int)cpu,
                    (unsigned int)base->active_count,
                    (unsigned long long)base->expired_count,
                    (unsigned long long)base->next_expires);
        }

#ifdef CONFIG_SMALL_MEMORY
        list_for_each_entry(timer, &base->active_list, list) {
            u32 rem = timer_remaining_ticks(timer);
            const char *mode = ((timer->flags & TIMER_FLAG_PERIODIC) != 0U) ?
                               "periodic" : "oneshot";
            if (print_fn != NULL) {
                print_fn("  timer[%s]: mode=%s period=%u rem_ticks=%u expires=%llu\n",
                         timer->name ? timer->name : "-",
                         mode,
                         (unsigned int)timer->init_tick,
                         (unsigned int)rem,
                         (unsigned long long)timer->timeout_tick);
            } else {
                pr_info("  timer[%s]: mode=%s period=%u rem_ticks=%u expires=%llu\n",
                        timer->name ? timer->name : "-",
                        mode,
                        (unsigned int)timer->init_tick,
                        (unsigned int)rem,
                        (unsigned long long)timer->timeout_tick);
            }
        }
#else
        {
            uint64_t map = base->bucket_bitmap;
            while (map != 0ULL) {
                uint32_t bit = (uint32_t)__builtin_ctzll(map);
                list_for_each_entry(timer, &base->buckets[bit], list) {
                    u32 rem = timer_remaining_ticks(timer);
                    const char *mode = ((timer->flags & TIMER_FLAG_PERIODIC) != 0U) ?
                                       "periodic" : "oneshot";
                    if (print_fn != NULL) {
                        print_fn("  timer[%s]: bucket=%u mode=%s period=%u rem_ticks=%u expires=%llu\n",
                                 timer->name ? timer->name : "-",
                                 (unsigned int)bit,
                                 mode,
                                 (unsigned int)timer->init_tick,
                                 (unsigned int)rem,
                                 (unsigned long long)timer->timeout_tick);
                    } else {
                        pr_info("  timer[%s]: bucket=%u mode=%s period=%u rem_ticks=%u expires=%llu\n",
                                timer->name ? timer->name : "-",
                                (unsigned int)bit,
                                mode,
                                (unsigned int)timer->init_tick,
                                (unsigned int)rem,
                                (unsigned long long)timer->timeout_tick);
                    }
                }
                map &= (map - 1ULL);
            }
        }
#endif
        spin_unlock_irq(&base->lock);
    }
}

void dump_timer(void)
{
    dump_timer_to(NULL);
}
