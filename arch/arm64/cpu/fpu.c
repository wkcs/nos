/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 惰性 FP / NEON / SVE 上下文框架
 * EL1 编译采用 -mgeneral-regs-only，默认屏蔽 CPACR_EL1.FPEN，按需陷阱激活。
 */

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <asm/sysreg.h>
#include <asm/percpu.h>

void arm64_fpu_init_cpu(void)
{
#if defined(__aarch64__)
    /* 初始屏蔽 EL0/EL1 对 FP/ASIMD 的访问，直到任务实际使用触发惰性加载 */
    struct arm64_percpu *pcpu = arm64_this_percpu();
    uint64_t cpacr = read_sysreg(cpacr_el1);
    cpacr &= ~CPACR_EL1_FPEN_MASK;
    cpacr |= CPACR_EL1_FPEN_TRAP;
    write_sysreg(cpacr, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");
    if (pcpu != NULL) {
        pcpu->fpu_owner = NULL;
    }
#endif
}

void arm64_fpu_trap_handler(void)
{
#if defined(__aarch64__)
    struct arm64_percpu *pcpu = arm64_this_percpu();
    struct task_struct *task = current;
    uint64_t cpacr = read_sysreg(cpacr_el1);

    cpacr &= ~CPACR_EL1_FPEN_MASK;
    cpacr |= CPACR_EL1_FPEN_FULL;
    write_sysreg(cpacr, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");

    if (task != NULL) {
        if (__atomic_load_n(&task->fp_state.valid, __ATOMIC_ACQUIRE) != 0U) {
            arm64_fp_restore(&task->fp_state);
        } else {
            arm64_fp_zero();
            __atomic_store_n(&task->fp_state.valid, 1U, __ATOMIC_RELEASE);
        }
        if (pcpu != NULL) {
            pcpu->fpu_owner = task;
        }
    }
#endif
}

void arm64_fpu_on_context_switch(struct task_struct *next)
{
#if defined(__aarch64__)
    struct arm64_percpu *pcpu = arm64_this_percpu();
    uint64_t cpacr = read_sysreg(cpacr_el1);

    /*
     * 仅当当前核上确有其它任务实际激活过 NEON/FP 时才在切出前保存寄存器，
     * 随后重设 FPEN 陷阱。这样未碰过向量寄存器的控制线程 0 额外开销，
     * 同时已脏任务切出后状态立即落回 task->fp_state，支持零 IPI 跨核迁移。
     */
    if (pcpu != NULL && pcpu->fpu_owner != NULL && pcpu->fpu_owner != next) {
        struct task_struct *owner = pcpu->fpu_owner;
        if ((cpacr & CPACR_EL1_FPEN_MASK) != CPACR_EL1_FPEN_FULL) {
            cpacr = (cpacr & ~CPACR_EL1_FPEN_MASK) | CPACR_EL1_FPEN_FULL;
            write_sysreg(cpacr, cpacr_el1);
            __asm__ volatile("isb" ::: "memory");
        }
        arm64_fp_save(&owner->fp_state);
        __atomic_store_n(&owner->fp_state.valid, 1U, __ATOMIC_RELEASE);
        pcpu->fpu_owner = NULL;
    }

    if (pcpu != NULL && pcpu->fpu_owner == next && next != NULL) {
        cpacr = (cpacr & ~CPACR_EL1_FPEN_MASK) | CPACR_EL1_FPEN_FULL;
    } else {
        cpacr = (cpacr & ~CPACR_EL1_FPEN_MASK) | CPACR_EL1_FPEN_TRAP;
    }
    write_sysreg(cpacr, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");
#else
    (void)next;
#endif
}
