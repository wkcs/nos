/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 任务栈初始化、双路径同步/中断延迟上下文切换与首次调度锁交接
 */

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <nos/sched.h>
#include <nos/hal_arch.h>
#include <asm/cpu.h>
#include <asm/switch.h>
#include <asm/percpu.h>
#include <asm/irq.h>
#include <asm/security.h>
#include <string.h>

extern void arm64_fpu_on_context_switch(struct task_struct *next);

addr_t *stack_init(void *task_entry, void *parameter, addr_t *stack_addr, void *task_exit)
{
    uintptr_t top;
    struct arm64_switch_frame *frame;

    /* task_create 传入的 stack_addr = stack_base + stack_size - sizeof(addr_t) */
    top = ((uintptr_t)stack_addr + sizeof(addr_t)) & ~(uintptr_t)0xFULL;
    frame = (struct arm64_switch_frame *)(top - sizeof(struct arm64_switch_frame));
    memset(frame, 0, sizeof(*frame));

    frame->x19 = (uint64_t)(uintptr_t)task_entry;
    frame->x20 = (uint64_t)(uintptr_t)parameter;
    frame->x21 = (uint64_t)(uintptr_t)task_exit;
    frame->x29 = 0ULL;
    frame->x30 = (uint64_t)(uintptr_t)arm64_task_trampoline;
    frame->tpidr_el0 = 0ULL;

    return (addr_t *)frame;
}

/* 新创建任务首次上核：完成与 switch_task() 的锁状态交接并确保开启中断 */
void arm64_finish_first_switch(void)
{
    struct sched_rq *rq = this_rq();

    /* 必须在持锁关中断状态下完成 FPU 拥有者与 PAC 密钥切换，防止开启中断后发生同核竞态 */
    arm64_fpu_on_context_switch(rq->curr);
    arm64_security_on_context_switch(rq->curr);

    if (spin_is_locked(&rq->lock)) {
        spin_unlock_irq(&rq->lock);
    } else {
        kernel_running = 1U;
    }

    arch_irq_enable_global();
}

void arm64_do_context_switch(addr_t from_sp_ptr, addr_t to_sp_ptr)
{
    addr_t saved_flags = *__nos_irq_flags_ptr();
    struct sched_rq *rq = this_rq();

    arm64_fpu_on_context_switch(rq->curr);
    arm64_security_on_context_switch(rq->curr);
    __arm64_cpu_switch_to(from_sp_ptr, to_sp_ptr);

    /* 恢复后在当前任务栈上将切走前的 IRQ 标志写回当前 CPU 槽（天然兼容 SMP 迁移） */
    *__nos_irq_flags_ptr() = saved_flags;
    arm64_fpu_on_context_switch(this_rq()->curr);
    arm64_security_on_context_switch(this_rq()->curr);
}

void context_switch(addr_t from, addr_t to)
{
    if (hal_in_irq()) {
        struct arm64_percpu *pcpu = arm64_this_percpu();
        if (!pcpu->deferred_switch) {
            pcpu->deferred_switch = true;
            pcpu->deferred_from = from;
        }
        pcpu->deferred_to = to;
        return;
    }
    arm64_do_context_switch(from, to);
}

void context_switch_interrupt(addr_t from, addr_t to)
{
    context_switch(from, to);
}

void context_switch_to(addr_t to)
{
    struct sched_rq *rq = this_rq();
    kernel_running = 1U;
    arm64_fpu_on_context_switch(rq->curr);
    arm64_security_on_context_switch(rq->curr);
    __arm64_cpu_switch_first(to);
}
