/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 多核 PSCI SMP 引导与核间调度通知
 */

#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <nos/sched.h>
#include <nos/hal_arch.h>
#include <nos/devicetree.h>
#include <asm/cpufeature.h>
#include <asm/gic.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#include <asm/percpu.h>
#include <asm/psci.h>
#include <asm/sysreg.h>
#include <stdio.h>

extern void arm64_vectors(void);
extern void arm64_fpu_init_cpu(void);
extern void arm64_timer_init_secondary(void);

extern const uintptr_t arm64_secondary_entry_phys;
extern const uintptr_t arm64_smp_boot_ttbr1_phys;
extern const uintptr_t arm64_smp_boot_stacks_phys;

static uint32_t g_smp_online_cpus = 1U;

static void arm64_ipi_resched_handler(uint32_t intid, void *ctx)
{
    (void)intid;
    (void)ctx;
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    if (kernel_running) {
        this_rq()->need_resched = true;
    }
#endif
}

/* 查询当前系统已成功上线的物理核总数 */
uint32_t arm64_smp_online_count(void)
{
    return __atomic_load_n(&g_smp_online_cpus, __ATOMIC_ACQUIRE);
}

/* 从设备树探测逻辑核对应的硬件 MPIDR reg；未命中则回退连续核号 */
static uint64_t smp_resolve_cpu_mpidr(uint32_t cpu_id)
{
    char path[32];
    const struct nos_dt_node *cpu_node;
    uint32_t reg_val = 0U;

    snprintf(path, sizeof(path), "/cpus/cpu@%u", (unsigned int)cpu_id);
    cpu_node = nos_dt_find_path(path);
    if (cpu_node != NULL && nos_dt_read_u32(cpu_node, "reg", 0U, &reg_val) == 0) {
        return (uint64_t)reg_val;
    }
    return (uint64_t)cpu_id;
}

/* 由 start.S 降级到 EL1h 并启用共享 TTBR1 后跳转进入的从核 C 主入口 */
void arm64_secondary_main(uint32_t cpu_id)
{
#if defined(__aarch64__)
    write_sysreg((uint64_t)(uintptr_t)&arm64_vectors, vbar_el1);
    /* 立即清空从核早期使用的临时 TTBR0_EL1，保证各核独立隔离 */
    __asm__ volatile("msr ttbr0_el1, xzr\n\t"
                     "tlbi vmalle1\n\t"
                     "dsb sy\n\t"
                     "isb"
                     ::: "memory");
#endif

    arm64_percpu_init(cpu_id);
    arm64_fpu_init_cpu();
    arm64_gic_cpu_init(cpu_id);
    arm64_timer_init_secondary();

#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    if (cpu_id < (uint32_t)CONFIG_NR_CPUS) {
        struct sched_rq *rq = sched_rq_of(cpu_id);
        uint64_t now_us = cpu_run_time_us();

        rq->clock_us = now_us;
        rq->win_start_us = (uint32_t)now_us;
        if (rq->curr != NULL) {
            rq->curr->start_time = now_us;
            rq->curr->se.exec_start_us = now_us;
            rq->curr->se.win_start_us = (uint32_t)now_us;
        }
        rq->online = true;
    }
#endif

    __atomic_store_n(&arm64_percpu_of(cpu_id)->online, true, __ATOMIC_RELEASE);
    arch_irq_enable_global();

    while (1) {
#if defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
        if (this_rq()->need_resched && scheduler_lock_nest == 0U) {
            switch_task();
        }
#endif
        cpu_set_lpm();
    }
}

/* 在核 0 调度器启动后通过 PSCI CPU_ON 拉起所有可用从核 */
void arch_smp_boot_secondary_cpus(void)
{
#if defined(CONFIG_SMP) && defined(CONFIG_NR_CPUS) && (CONFIG_NR_CPUS > 1)
    static const char *const idle_names[8] = {
        "idle/0", "idle/1", "idle/2", "idle/3",
        "idle/4", "idle/5", "idle/6", "idle/7",
    };
    volatile uint64_t *boot_ttbr1_va;
    volatile uint64_t *boot_stacks_va;
    uint32_t online = 1U;
    uint32_t max_cpus = (uint32_t)CONFIG_NR_CPUS;

    if (max_cpus > 8U) {
        max_cpus = 8U;
    }

    (void)nos_irq_register(NOS_IPI_RESCHED, arm64_ipi_resched_handler, NULL);
    arm64_gic_set_priority(NOS_IPI_RESCHED, 0x60U);
    arm64_gic_enable_irq(NOS_IPI_RESCHED);

    boot_ttbr1_va = (volatile uint64_t *)arm64_phys_to_virt(arm64_smp_boot_ttbr1_phys);
    boot_stacks_va = (volatile uint64_t *)arm64_phys_to_virt(arm64_smp_boot_stacks_phys);
    *boot_ttbr1_va = (uint64_t)arm64_mmu_get_kernel_ttbr1();
    arm64_clean_dcache_poc((uintptr_t)boot_ttbr1_va, sizeof(uint64_t));

    for (uint32_t cpu = 1U; cpu < max_cpus; cpu++) {
        uint64_t target_mpidr = smp_resolve_cpu_mpidr(cpu);
        struct task_struct *idle_task;
        uintptr_t stack_top;
        int psci_rc;
        uint32_t waited_ms = 0U;

        idle_task = task_create_ex(idle_names[cpu],
                                   NULL,
                                   NULL,
                                   SCHED_POLICY_IDLE,
                                   IDEL_TASK_PRIO,
                                   0,
                                   (uint8_t)(1U << cpu),
                                   CONFIG_IDEL_TASK_STACK_SIZE,
                                   10U,
                                   NULL);
        if (idle_task == NULL) {
            pr_err("ARM64 SMP: failed to allocate idle task for CPU%u\n",
                   (unsigned int)cpu);
            break;
        }

        idle_task->cpu = (uint8_t)cpu;
        idle_task->status = TASK_RUNNING;
        idle_task->on_cpu = 1U;
        idle_task->sched_class = &idle_sched_class;
        g_sched_rqs[cpu].idle = idle_task;
        g_sched_rqs[cpu].curr = idle_task;

        stack_top = ((uintptr_t)idle_task->stack + sizeof(addr_t)) & ~(uintptr_t)0xFULL;
        boot_stacks_va[cpu] = (uint64_t)stack_top;
        arm64_clean_dcache_poc((uintptr_t)&boot_stacks_va[cpu], sizeof(uint64_t));

        psci_rc = psci_cpu_on(target_mpidr, arm64_secondary_entry_phys, (uint64_t)cpu);
        if (psci_rc != PSCI_RET_SUCCESS) {
            g_sched_rqs[cpu].idle = NULL;
            g_sched_rqs[cpu].curr = NULL;
            idle_task->on_cpu = 0U;
            idle_task->status = TASK_SUSPENDED;
            task_del(idle_task);
            continue;
        }

        while (!__atomic_load_n(&arm64_percpu_of(cpu)->online, __ATOMIC_ACQUIRE) &&
               waited_ms < 200U) {
            cpu_delay_us(1000U);
            waited_ms++;
        }

        if (__atomic_load_n(&arm64_percpu_of(cpu)->online, __ATOMIC_ACQUIRE)) {
            online++;
        } else {
            pr_err("ARM64 SMP: CPU%u timed out waiting to come online\n",
                   (unsigned int)cpu);
        }
    }

    __atomic_store_n(&g_smp_online_cpus, online, __ATOMIC_RELEASE);
    pr_info("ARM64 SMP: %u/%u CPUs online\n",
            (unsigned int)online, (unsigned int)max_cpus);
#endif
}
