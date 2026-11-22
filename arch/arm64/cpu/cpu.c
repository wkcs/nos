/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 CPU 抽象层核心控制与内核高半核引导入口
 */

#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/unwind.h>
#include <nos/hal_arch.h>
#include <asm/init.h>
#include <asm/mmu.h>
#include <asm/cpufeature.h>
#include <asm/percpu.h>
#include <asm/psci.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

extern void arm64_vectors(void);
extern void arm64_fpu_init_cpu(void);
extern void arm64_timer_init_hz(uint32_t hz);
extern u64 arm64_timer_monotonic_us(void);
extern void arm64_timer_delay_ns(uint64_t ns);
extern int nos_start(void);

static uint64_t g_boot_dtb_phys;
static uint8_t g_boot_el = 1U;

uint64_t arm64_get_boot_dtb_phys(void)
{
    return g_boot_dtb_phys;
}

uint8_t arm64_get_boot_el(void)
{
    return g_boot_el;
}

/* 由 start.S 开启 MMU 后跳转到高半核的第一个 C 入口 */
void nos_arm64_boot(uint64_t dtb_pa, uint64_t boot_el)
{
    g_boot_dtb_phys = dtb_pa;
    g_boot_el = (uint8_t)boot_el;

#if defined(__aarch64__)
    write_sysreg((uint64_t)(uintptr_t)&arm64_vectors, vbar_el1);
    __asm__ volatile("isb" ::: "memory");
#endif

    arm64_percpu_init(0U);
    arm64_cpufeature_init((uint8_t)boot_el);
    arm64_fpu_init_cpu();
    arm64_mmu_init_kernel_wx();

    (void)nos_start();
    while (1) {
#if defined(__aarch64__)
        __asm__ volatile("wfi" ::: "memory");
#endif
    }
}

__init void asm_cpu_init(void)
{
#ifdef CONFIG_SYS_TICK_MS
    uint32_t hz = (CONFIG_SYS_TICK_MS > 0U) ? (1000U / CONFIG_SYS_TICK_MS) : 1000U;
#else
    uint32_t hz = 1000U;
#endif
    arm64_timer_init_hz(hz);
}

void asm_cpu_delay_ns(uint32_t ns)
{
    arm64_timer_delay_ns((uint64_t)ns);
}

void asm_cpu_delay_us(uint32_t us)
{
    arm64_timer_delay_ns((uint64_t)us * 1000ULL);
}

void asm_cpu_reboot(void)
{
    psci_system_reset();
}

u64 asm_cpu_run_time_us(void)
{
    return arm64_timer_monotonic_us();
}

void asm_cpu_set_lpm(void)
{
#if defined(__aarch64__)
    __asm__ volatile("wfi" ::: "memory");
#endif
}

void arch_dump_stack(void)
{
    dump_task_stack(current, NULL);
}
