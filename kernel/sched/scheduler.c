/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
uint64_t kernel_ticks;
unsigned timer_switches, reschedule;
struct task *cpu_tasks[NOS_CPUS];
int kernel_clock_snapshot(uintptr_t clock, struct nos_clock_value *value)
{
    if (clock == 1 || clock == 4 || clock == 6 || clock == 7) {
        value->seconds = (int64_t)(kernel_ticks / 100);
        value->nanoseconds = (int64_t)(kernel_ticks % 100) * 10000000; return 0;
    }
    if (clock != 0 && clock != 5) return -NOS_ENOTSUP;
    uint64_t ns = arch_realtime_ns(); if (!ns) return -NOS_ENOTSUP;
    static uint64_t rtc_base, rtc_tick, previous;
    if (!rtc_base) { rtc_base = ns; rtc_tick = kernel_ticks; }
    uint64_t interpolated = rtc_base + (kernel_ticks - rtc_tick) * 10000000;
    if (ns > interpolated) { rtc_base = ns; rtc_tick = kernel_ticks; interpolated = ns; }
    if (interpolated < previous) interpolated = previous;
    previous = interpolated;
    value->seconds = (int64_t)(interpolated / 1000000000);
    value->nanoseconds = (int64_t)(interpolated % 1000000000); return 0;
}
struct task *select_task(void)
{
    struct task *next = NULL;
    unsigned cpu = arch_cpu_id();
    unsigned start = current_task ? current_task->id % NOS_TASKS : NOS_TASKS - 1;
    for (unsigned n = 1; n <= NOS_TASKS; n++) {
        struct task *t = &tasks[(start + n) % NOS_TASKS];
        if (
#ifdef NOS_MMU
            !t->linux_stopped &&
#endif
            (t->pin_cpu == UINT32_MAX || t->pin_cpu == cpu) && t->state == TASK_READY && (t->run_cpu == UINT32_MAX || t->run_cpu == cpu) &&
            (!next || t->priority < next->priority)) next = t;
    }
    return next;
}
void scheduler_tick(void)
{
    if (arch_cpu_id() == 0) kernel_ticks++;
    unsigned futex_collected = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEP && t->wake_tick <= kernel_ticks) t->state = TASK_READY;
        if (t->state == TASK_FUTEX && t->wake_tick && t->wake_tick <= kernel_ticks) {
            FRAME_RESULT(&t->frame) = (uintptr_t)-NOS_ETIMEDOUT; t->state = TASK_READY;
            futex_collected = 1;
        }
    }
    if (futex_collected) vm_shared_collect();
    ipc_timeouts(); irq_timeouts(); linux_signal_tick(); linux_timer_tick(); linux_exec_progress();
}
