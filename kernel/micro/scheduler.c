/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
uint64_t kernel_ticks;
unsigned timer_switches, reschedule;
struct task *cpu_tasks[NOS_CPUS];
struct task *select_task(void)
{
    struct task *next = NULL;
    unsigned cpu = arch_cpu_id();
    unsigned start = current_task ? current_task->id % NOS_TASKS : NOS_TASKS - 1;
    for (unsigned n = 1; n <= NOS_TASKS; n++) {
        struct task *t = &tasks[(start + n) % NOS_TASKS];
        if ((t->pin_cpu == UINT32_MAX || t->pin_cpu == cpu) && t->state == TASK_READY && (t->run_cpu == UINT32_MAX || t->run_cpu == cpu) &&
            (!next || t->priority < next->priority)) next = t;
    }
    return next;
}
void scheduler_tick(void)
{
    if (arch_cpu_id() == 0) kernel_ticks++;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEP && t->wake_tick <= kernel_ticks) t->state = TASK_READY;
        if (t->state == TASK_FUTEX && t->wake_tick && t->wake_tick <= kernel_ticks) {
            FRAME_RESULT(&t->frame) = (uintptr_t)-NOS_ETIMEDOUT; t->state = TASK_READY;
        }
    }
    ipc_timeouts(); irq_timeouts();
}
