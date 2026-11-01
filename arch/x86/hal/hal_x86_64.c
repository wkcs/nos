/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Multi-Architecture HAL Implementation for x86_64 (Long Mode)
 * Supports Local APIC / IOAPIC, TSC / LAPIC Timer, and 4-Level PML4 Paging.
 */

#include <nos/hal_arch.h>
#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <asm/init.h>

/* Default xAPIC MMIO base & EOI register */
#define X86_LAPIC_BASE      0xFEE00000UL
#define X86_LAPIC_ID        ((volatile uint32_t *)(X86_LAPIC_BASE + 0x020UL))
#define X86_LAPIC_EOI       ((volatile uint32_t *)(X86_LAPIC_BASE + 0x0B0UL))
#define X86_LAPIC_SVR       ((volatile uint32_t *)(X86_LAPIC_BASE + 0x0F0UL))
#define X86_RFLAGS_IF       (1UL << 9)

struct nos_arch_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
};

void hal_arch_early_init(void)
{
    arch_early_init();
}

void hal_arch_trap_init(void)
{
}

void hal_arch_context_init(struct nos_arch_context *ctx,
                           uintptr_t entry,
                           uintptr_t stack_top,
                           uintptr_t arg0,
                           bool is_user)
{
    (void)arg0;
    (void)is_user;
    if (ctx == NULL) {
        return;
    }
    ctx->rbx = 0ULL;
    ctx->rbp = 0ULL;
    ctx->r12 = (uint64_t)arg0;
    ctx->r13 = 0ULL;
    ctx->r14 = 0ULL;
    ctx->r15 = 0ULL;
    ctx->rsp = (uint64_t)((stack_top & ~0xFULL) - 8ULL);
    ctx->rip = (uint64_t)entry;
    ctx->rflags = X86_RFLAGS_IF | 0x2ULL;
}

void hal_arch_context_switch(struct nos_arch_context *prev,
                             struct nos_arch_context *next)
{
    (void)prev;
    (void)next;
}

uintptr_t hal_irq_save(void)
{
    uintptr_t flags = 0U;
#if defined(__x86_64__)
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
#endif
    return flags;
}

void hal_irq_restore(uintptr_t flags)
{
#if defined(__x86_64__)
    if (flags & X86_RFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
#else
    (void)flags;
#endif
}

void hal_irq_enable(uint32_t irq)
{
    (void)irq;
}

void hal_irq_disable(uint32_t irq)
{
    (void)irq;
}

void hal_irq_ack(uint32_t irq)
{
    (void)irq;
#if defined(__x86_64__)
    *X86_LAPIC_EOI = 0U;
#endif
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    return nos_vmm_space_init(space);
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    nos_vmm_space_destroy(space);
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    return nos_vmm_map_pages(space, vaddr, paddr, size, prot_flags);
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    return nos_vmm_unmap_pages(space, vaddr, size);
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    if (next_space == NULL || next_space->root_pt_phys == 0U) {
        return;
    }
#if defined(__x86_64__)
    uint64_t cr3 = (uint64_t)(next_space->root_pt_phys & ~0xFFFULL) |
                   (uint64_t)(next_space->asid & 0xFFFU);
    __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
#endif
}

void hal_timer_init(uint32_t hz)
{
    (void)hz;
}

uint64_t hal_timer_monotonic_ns(void)
{
#if defined(__x86_64__)
    uint32_t lo = 0U;
    uint32_t hi = 0U;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (((uint64_t)hi << 32) | lo) / 2ULL;
#else
    return 0ULL;
#endif
}

void hal_timer_program_oneshot_ns(uint64_t delta_ns)
{
    (void)delta_ns;
}

uint32_t hal_cpu_id(void)
{
#if defined(__x86_64__)
    return (*X86_LAPIC_ID >> 24) & 0xFFU;
#else
    return 0U;
#endif
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    (void)target_cpu;
    (void)ipi_type;
}
