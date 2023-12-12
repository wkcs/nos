/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/sleep.h>
#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/sch.h>

void nsleep(u32 nsec)
{
    cpu_delay_ns(nsec);
}

void usleep(u32 usec)
{
    cpu_delay_us(usec);
}

void msleep(u32 msec)
{
    u64 run_times;

    run_times = cpu_run_time_us();
    if (run_times - sys_heartbeat_time > (CONFIG_SYS_TICK_MS * 500)) {
        msec = (msec + CONFIG_SYS_TICK_MS / 2) / CONFIG_SYS_TICK_MS;
    } else {
        msec = (msec + CONFIG_SYS_TICK_MS - 1) / CONFIG_SYS_TICK_MS;
    }
    task_sleep(msec);
}

void sleep(u32 sec)
{
    msleep(sec * 1000);
}
