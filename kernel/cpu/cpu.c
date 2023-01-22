/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/cpu.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <asm/cpu.h>

static u64 run_ticks;

__init void cpu_init(void)
{
    asm_cpu_init();
}

void system_beat_processing(void)
{
    if (run_ticks < U64_MAX)
        run_ticks++;
    else
        run_ticks = 0;
}

u64 cpu_run_ticks(void)
{
    return run_ticks;
}

u64 cpu_run_time_us(void)
{
    return asm_cpu_run_time_us();
}

void cpu_reboot(void)
{
    asm_cpu_reboot();
}

void cpu_delay_us(u32 us)
{
    asm_cpu_delay_us(us);
}
