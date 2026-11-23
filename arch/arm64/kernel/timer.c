/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM Generic Timer 驱动（128-bit 纳秒换算防溢出 + CNTP_CVAL_EL0 无漂移周期心跳）
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <asm/irq.h>

#ifndef __HOST_TEST__
#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <asm/sysreg.h>
#include <asm/gic.h>
#include <asm/percpu.h>
#endif

#define ARM64_TIMER_PHYS_PPI    30U

/* 使用 128 位中间算术，避免 64 位 (ticks * 1e9) 在运行数分钟后溢出 */
uint64_t arm64_timer_ticks_to_ns(uint64_t ticks, uint64_t freq)
{
    if (freq == 0ULL) {
        return 0ULL;
    }
    __uint128_t num = (__uint128_t)ticks * (__uint128_t)1000000000ULL;
    return (uint64_t)(num / freq);
}

uint64_t arm64_timer_ns_to_ticks(uint64_t ns, uint64_t freq)
{
    if (freq == 0ULL) {
        return 0ULL;
    }
    __uint128_t num = (__uint128_t)ns * (__uint128_t)freq;
    return (uint64_t)(num / 1000000000ULL);
}

#ifndef __HOST_TEST__
static uint64_t g_timer_freq;
static uint64_t g_tick_interval;
static uint64_t g_next_tick_cval[CONFIG_NR_CPUS];
static uint64_t g_oneshot_cval[CONFIG_NR_CPUS];
static bool g_timer_registered;

static inline uint32_t timer_cpu_idx(void)
{
    uint32_t cpu = arm64_this_percpu()->cpu_id;
    return (cpu < (uint32_t)CONFIG_NR_CPUS) ? cpu : 0U;
}

static inline uint64_t read_cntpct(void)
{
    uint64_t val = 0ULL;
#if defined(__aarch64__)
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(val));
#endif
    return val;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t val = 0ULL;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
#endif
    return val;
}

static void timer_rearm_hw_cval(uint32_t cpu)
{
    uint64_t target = g_next_tick_cval[cpu];

    if (g_oneshot_cval[cpu] != 0ULL &&
        (target == 0ULL || g_oneshot_cval[cpu] < target)) {
        target = g_oneshot_cval[cpu];
    }
#if defined(__aarch64__)
    write_sysreg(target, cntp_cval_el0);
    __asm__ volatile("isb" ::: "memory");
#else
    (void)target;
#endif
}

static void arm64_timer_irq_handler(uint32_t intid, void *ctx)
{
    uint32_t cpu = timer_cpu_idx();
    uint64_t now = read_cntpct();
    bool tick_due = (g_tick_interval > 0ULL &&
                     g_next_tick_cval[cpu] != 0ULL &&
                     now >= g_next_tick_cval[cpu]);
    bool oneshot_due = (g_oneshot_cval[cpu] != 0ULL && now >= g_oneshot_cval[cpu]);
    (void)intid;
    (void)ctx;

    if (!tick_due && !oneshot_due && g_tick_interval > 0ULL) {
        tick_due = true;
    }

    if (oneshot_due) {
        g_oneshot_cval[cpu] = 0ULL;
    }
    if (tick_due && g_tick_interval > 0ULL) {
        g_next_tick_cval[cpu] += g_tick_interval;
        if (g_next_tick_cval[cpu] <= now) {
            g_next_tick_cval[cpu] = now + g_tick_interval;
        }
    }
    timer_rearm_hw_cval(cpu);

    if (tick_due) {
        if (cpu == 0U) {
            system_heartbeat_process();
        } else if (kernel_running) {
            /* 从核不重复递增全局 run_ticks，仅驱动本核调度节拍与 Per-CPU 定时器 */
            sch_heartbeat();
            timer_check_handle();
        }
    } else if (oneshot_due && kernel_running) {
        /* 微秒级精准 oneshot 抢占路径：不触发周期系统时钟增量，严格防止时钟漂移 */
        sch_heartbeat();
    }
}

void arm64_timer_init_hz(uint32_t hz)
{
    uint32_t cpu = timer_cpu_idx();
    uint64_t now;

    g_timer_freq = read_cntfrq();
    if (g_timer_freq == 0ULL) {
        g_timer_freq = 62500000ULL;
    }
    if (hz == 0U) {
        hz = 1000U;
    }

    g_tick_interval = g_timer_freq / (uint64_t)hz;
    if (g_tick_interval == 0ULL) {
        g_tick_interval = 1ULL;
    }

    if (!g_timer_registered) {
        (void)nos_irq_register(ARM64_TIMER_PHYS_PPI, arm64_timer_irq_handler, NULL);
        g_timer_registered = true;
    }

    now = read_cntpct();
    g_next_tick_cval[cpu] = now + g_tick_interval;
    g_oneshot_cval[cpu] = 0ULL;
    timer_rearm_hw_cval(cpu);
#if defined(__aarch64__)
    write_sysreg(1ULL, cntp_ctl_el0);
    __asm__ volatile("isb" ::: "memory");
#endif
    arm64_gic_enable_irq(ARM64_TIMER_PHYS_PPI);
}

void arm64_timer_enable_irq(void)
{
    uint32_t cpu = timer_cpu_idx();
    uint64_t now;

    if (g_tick_interval == 0ULL) {
        return;
    }
    now = read_cntpct();
    g_next_tick_cval[cpu] = now + g_tick_interval;
    g_oneshot_cval[cpu] = 0ULL;
    timer_rearm_hw_cval(cpu);
#if defined(__aarch64__)
    write_sysreg(1ULL, cntp_ctl_el0);
    __asm__ volatile("isb" ::: "memory");
#endif
    arm64_gic_set_priority(ARM64_TIMER_PHYS_PPI, 0x80U);
    arm64_gic_enable_irq(ARM64_TIMER_PHYS_PPI);
}

void arm64_timer_init_secondary(void)
{
    arm64_timer_enable_irq();
}

uint64_t arm64_timer_monotonic_ns(void)
{
    uint64_t freq = g_timer_freq ? g_timer_freq : read_cntfrq();
    return arm64_timer_ticks_to_ns(read_cntpct(), freq);
}

u64 arm64_timer_monotonic_us(void)
{
    return (u64)(arm64_timer_monotonic_ns() / 1000ULL);
}

void arm64_timer_program_oneshot_ns(uint64_t delta_ns)
{
    uint32_t cpu = timer_cpu_idx();
    uint64_t freq = g_timer_freq ? g_timer_freq : read_cntfrq();
    uint64_t ticks = arm64_timer_ns_to_ticks(delta_ns, freq);
    if (ticks == 0ULL) {
        ticks = 1ULL;
    }
    g_oneshot_cval[cpu] = read_cntpct() + ticks;
    timer_rearm_hw_cval(cpu);
#if defined(__aarch64__)
    write_sysreg(1ULL, cntp_ctl_el0);
    __asm__ volatile("isb" ::: "memory");
#endif
}

void arm64_timer_delay_ns(uint64_t ns)
{
    uint64_t freq = g_timer_freq ? g_timer_freq : read_cntfrq();
    uint64_t ticks = arm64_timer_ns_to_ticks(ns, freq);
    uint64_t start = read_cntpct();

    while ((read_cntpct() - start) < ticks) {
#if defined(__aarch64__)
        __asm__ volatile("nop");
#endif
    }
}
#endif /* !__HOST_TEST__ */
