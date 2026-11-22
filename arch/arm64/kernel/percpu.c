/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Per-CPU 运行时状态管理（绑定 TPIDR_EL1）
 */

#include <kernel/kernel.h>
#include <asm/percpu.h>
#include <asm/sysreg.h>

static struct arm64_percpu g_arm64_percpus[CONFIG_NR_CPUS];

void arm64_percpu_init(uint32_t cpu_id)
{
    struct arm64_percpu *pcpu;
    uint64_t mpidr = 0ULL;

    if (cpu_id >= (uint32_t)CONFIG_NR_CPUS) {
        cpu_id = 0U;
    }
    pcpu = &g_arm64_percpus[cpu_id];

#if defined(__aarch64__)
    mpidr = read_sysreg(mpidr_el1);
    write_sysreg((uint64_t)(uintptr_t)pcpu, tpidr_el1);
    __asm__ volatile("isb" ::: "memory");
#endif

    pcpu->cpu_id = cpu_id;
    pcpu->mpidr = mpidr;
    pcpu->cpu_capacity = 1024U;
    pcpu->deferred_switch = false;
    pcpu->deferred_from = 0U;
    pcpu->deferred_to = 0U;
    pcpu->fpu_owner = NULL;
    pcpu->online = true;
}

struct arm64_percpu *arm64_this_percpu(void)
{
#if defined(__aarch64__)
    struct arm64_percpu *pcpu = (struct arm64_percpu *)(uintptr_t)read_sysreg(tpidr_el1);
    if (pcpu != NULL) {
        return pcpu;
    }
#endif
    return &g_arm64_percpus[0];
}

struct arm64_percpu *arm64_percpu_of(uint32_t cpu_id)
{
    if (cpu_id >= (uint32_t)CONFIG_NR_CPUS) {
        return &g_arm64_percpus[0];
    }
    return &g_arm64_percpus[cpu_id];
}
