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
    uint64_t cpacr = read_sysreg(cpacr_el1);
    cpacr &= ~CPACR_EL1_FPEN_MASK;
    cpacr |= CPACR_EL1_FPEN_TRAP;
    write_sysreg(cpacr, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");
#endif
}

void arm64_fpu_trap_handler(void)
{
#if defined(__aarch64__)
    struct arm64_percpu *pcpu = arm64_this_percpu();
    uint64_t cpacr = read_sysreg(cpacr_el1);
    cpacr &= ~CPACR_EL1_FPEN_MASK;
    cpacr |= CPACR_EL1_FPEN_FULL;
    write_sysreg(cpacr, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");

    if (pcpu != NULL && current != NULL) {
        pcpu->fpu_owner = current;
    }
#endif
}

void arm64_fpu_on_context_switch(struct task_struct *next)
{
#if defined(__aarch64__)
    struct arm64_percpu *pcpu = arm64_this_percpu();
    uint64_t cpacr = read_sysreg(cpacr_el1);

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
