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
#include <asm/sysreg.h>
#include <asm/gic.h>
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
static uint64_t g_next_cval;
static bool g_timer_registered;

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

static void arm64_timer_irq_handler(uint32_t intid, void *ctx)
{
    uint64_t now;
    (void)intid;
    (void)ctx;

    if (g_tick_interval > 0ULL) {
        g_next_cval += g_tick_interval;
        now = read_cntpct();
        if (g_next_cval <= now) {
            g_next_cval = now + g_tick_interval;
        }
#if defined(__aarch64__)
        write_sysreg(g_next_cval, cntp_cval_el0);
        __asm__ volatile("isb" ::: "memory");
#endif
    }
    system_heartbeat_process();
}

void arm64_timer_init_hz(uint32_t hz)
{
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
    g_next_cval = now + g_tick_interval;
#if defined(__aarch64__)
    write_sysreg(g_next_cval, cntp_cval_el0);
    write_sysreg(1ULL, cntp_ctl_el0);
    __asm__ volatile("isb" ::: "memory");
#endif
    arm64_gic_enable_irq(ARM64_TIMER_PHYS_PPI);
}

void arm64_timer_enable_irq(void)
{
    uint64_t now;

    if (g_tick_interval == 0ULL) {
        return;
    }
    now = read_cntpct();
    g_next_cval = now + g_tick_interval;
#if defined(__aarch64__)
    write_sysreg(g_next_cval, cntp_cval_el0);
    write_sysreg(1ULL, cntp_ctl_el0);
    __asm__ volatile("isb" ::: "memory");
#endif
    arm64_gic_set_priority(ARM64_TIMER_PHYS_PPI, 0x80U);
    arm64_gic_enable_irq(ARM64_TIMER_PHYS_PPI);
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
    uint64_t freq = g_timer_freq ? g_timer_freq : read_cntfrq();
    uint64_t ticks = arm64_timer_ns_to_ticks(delta_ns, freq);
    if (ticks == 0ULL) {
        ticks = 1ULL;
    }
    g_next_cval = read_cntpct() + ticks;
#if defined(__aarch64__)
    write_sysreg(g_next_cval, cntp_cval_el0);
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
