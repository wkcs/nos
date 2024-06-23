/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/cpu.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sch.h>
#include <kernel/printk.h>
#include <kernel/boot.h>
#include <kernel/timer.h>
#include <asm/cpu.h>
#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

static u64 run_ticks;

__init void cpu_init(void)
{
    asm_cpu_init();
}

void system_heartbeat_process(void)
{
    if (run_ticks < U64_MAX)
        run_ticks++;
    else
        run_ticks = 0;

    if (!kernel_running)
        return;

    sch_heartbeat();
    timer_check_handle();
#ifdef CONFIG_LVGL
    lv_tick_inc(CONFIG_SYS_TICK_MS);
#endif
}

u64 cpu_run_ticks(void)
{
    return run_ticks;
}

u64 cpu_run_time_us(void)
{
    return asm_cpu_run_time_us();
}

void cpu_reboot(u32 flag)
{
    write_boot_flag(flag);
    asm_cpu_reboot();
}

void cpu_delay_ns(u32 ns)
{
    asm_cpu_delay_ns(ns);
}

void cpu_delay_us(u32 us)
{
    asm_cpu_delay_us(us);
}

void cpu_set_lpm(void)
{
    asm_cpu_set_lpm();
}
