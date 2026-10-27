/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Multi-Architecture HAL Implementation for ARM (Cortex-M3/M4/v7-M)
 */

#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <asm/init.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#include <asm/task.h>
#include <kernel/cpu.h>

/* NVIC ICPR register for acknowledging/clearing pending external interrupts */
#define ARM_NVIC_ICPR_BASE ((volatile uint32_t *)0xE000E280UL)

struct nos_arch_context {
    uintptr_t sp;
};

struct nos_vm_space {
    uint32_t mpu_region_mask;
};

void hal_arch_early_init(void)
{
    arch_early_init();
}

void hal_arch_trap_init(void)
{
    arch_irq_init();
}

void hal_arch_context_init(struct nos_arch_context *ctx,
                           uintptr_t entry,
                           uintptr_t stack_top,
                           uintptr_t arg0,
                           bool is_user)
{
    (void)is_user;
    if (ctx == NULL) {
        return;
    }
    ctx->sp = (uintptr_t)stack_init((void *)entry,
                                    (void *)arg0,
                                    (addr_t *)stack_top,
                                    NULL);
}

void hal_arch_context_switch(struct nos_arch_context *prev,
                             struct nos_arch_context *next)
{
    if (next == NULL) {
        return;
    }
    if (prev == NULL) {
        context_switch_to((addr_t)&next->sp);
    } else {
        context_switch((addr_t)&prev->sp, (addr_t)&next->sp);
    }
}

uintptr_t hal_irq_save(void)
{
    return (uintptr_t)arch_irq_save();
}

void hal_irq_restore(uintptr_t flags)
{
    arch_irq_restore((unsigned long)flags);
}

void hal_irq_enable(uint32_t irq)
{
    arch_irq_enable((unsigned int)irq);
}

void hal_irq_disable(uint32_t irq)
{
    arch_irq_disable((unsigned int)irq);
}

void hal_irq_ack(uint32_t irq)
{
    if (irq < 240U) {
        ARM_NVIC_ICPR_BASE[irq / 32U] = (1UL << (irq % 32U));
    }
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    if (space != NULL) {
        space->mpu_region_mask = 0U;
    }
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL) {
        space->mpu_region_mask = 0U;
    }
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    (void)space;
    return arch_map_page((unsigned long)vaddr,
                         (unsigned long)paddr,
                         (unsigned long)size,
                         (unsigned int)prot_flags);
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    (void)space;
    return arch_unmap_page((unsigned long)vaddr, (unsigned long)size);
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    (void)next_space;
}

void hal_timer_init(uint32_t hz)
{
    (void)hz;
    cpu_init();
}

uint64_t hal_timer_monotonic_ns(void)
{
    return (uint64_t)cpu_run_time_us() * 1000ULL;
}

void hal_timer_program_oneshot_ns(uint64_t delta_ns)
{
    (void)delta_ns;
}

uint32_t hal_cpu_id(void)
{
    return 0U;
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    (void)target_cpu;
    (void)ipi_type;
}
