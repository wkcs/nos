/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Multi-Architecture HAL Implementation for ARM64 (ARMv8-A EL1/EL0)
 * Supports QEMU virt GICv2, ARM Generic Timer, and 4KB 4-level MMU page tables.
 */

#include <nos/hal_arch.h>
#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <asm/init.h>
#include <asm/irq.h>
#include <asm/mmu.h>

/* QEMU virt GICv2 Distributor & CPU Interface MMIO bases */
#define ARM64_GICD_BASE         0x08000000UL
#define ARM64_GICC_BASE         0x08010000UL
#define ARM64_UART_PL011_BASE   0x09000000UL

#define GICD_CTLR               ((volatile uint32_t *)(ARM64_GICD_BASE + 0x000UL))
#define GICD_ISENABLER(n)       ((volatile uint32_t *)(ARM64_GICD_BASE + 0x100UL + (4UL * (n))))
#define GICD_ICENABLER(n)       ((volatile uint32_t *)(ARM64_GICD_BASE + 0x180UL + (4UL * (n))))
#define GICC_CTLR               ((volatile uint32_t *)(ARM64_GICC_BASE + 0x000UL))
#define GICC_PMR                ((volatile uint32_t *)(ARM64_GICC_BASE + 0x004UL))
#define GICC_EOIR               ((volatile uint32_t *)(ARM64_GICC_BASE + 0x010UL))

struct nos_arch_context {
    uint64_t x19_to_x30[12];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t tpidr_el0;
};

void hal_arch_early_init(void)
{
    arch_early_init();
}

void hal_arch_trap_init(void)
{
    *GICD_CTLR = 1U;
    *GICC_PMR = 0xFFU;
    *GICC_CTLR = 1U;
}

void hal_arch_context_init(struct nos_arch_context *ctx,
                           uintptr_t entry,
                           uintptr_t stack_top,
                           uintptr_t arg0,
                           bool is_user)
{
    uint32_t i;

    if (ctx == NULL) {
        return;
    }
    for (i = 0; i < 12U; i++) {
        ctx->x19_to_x30[i] = 0ULL;
    }
    ctx->x19_to_x30[0] = (uint64_t)arg0;
    ctx->x19_to_x30[11] = (uint64_t)entry; /* x30 (LR) */
    ctx->sp = (uint64_t)(stack_top & ~0xFULL);
    ctx->pc = (uint64_t)entry;
    ctx->pstate = is_user ? 0x0ULL : 0x5ULL; /* EL0t vs EL1h */
    ctx->tpidr_el0 = 0ULL;
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
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(flags) :: "memory");
#endif
    return flags;
}

void hal_irq_restore(uintptr_t flags)
{
#if defined(__aarch64__)
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
#else
    (void)flags;
#endif
}

void hal_irq_enable(uint32_t irq)
{
    if (irq < 1020U) {
        *GICD_ISENABLER(irq / 32U) = (1U << (irq % 32U));
    }
}

void hal_irq_disable(uint32_t irq)
{
    if (irq < 1020U) {
        *GICD_ICENABLER(irq / 32U) = (1U << (irq % 32U));
    }
}

void hal_irq_ack(uint32_t irq)
{
    *GICC_EOIR = irq;
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
#if defined(__aarch64__)
    uint64_t ttbr = ((uint64_t)next_space->asid << 48) |
                    (uint64_t)(next_space->root_pt_phys & 0x0000FFFFFFFFF000ULL);
    __asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(ttbr) : "memory");
#endif
}

void hal_timer_init(uint32_t hz)
{
#if defined(__aarch64__)
    uint64_t freq = 0ULL;
    uint64_t tval;
    uint64_t ctl = 1ULL;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    tval = (hz > 0U && freq > 0ULL) ? (freq / hz) : 625000ULL;
    __asm__ volatile("msr cntp_tval_el0, %0\n\tmsr cntp_ctl_el0, %1\n\tisb"
                     :: "r"(tval), "r"(ctl) : "memory");
#else
    (void)hz;
#endif
}

uint64_t hal_timer_monotonic_ns(void)
{
#if defined(__aarch64__)
    uint64_t pct = 0ULL;
    uint64_t freq = 0ULL;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(pct));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0ULL) {
        return 0ULL;
    }
    return (pct * 1000000000ULL) / freq;
#else
    return 0ULL;
#endif
}

void hal_timer_program_oneshot_ns(uint64_t delta_ns)
{
#if defined(__aarch64__)
    uint64_t freq = 0ULL;
    uint64_t ticks;
    uint64_t ctl = 1ULL;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    ticks = (freq * delta_ns) / 1000000000ULL;
    if (ticks == 0ULL) {
        ticks = 1ULL;
    }
    __asm__ volatile("msr cntp_tval_el0, %0\n\tmsr cntp_ctl_el0, %1\n\tisb"
                     :: "r"(ticks), "r"(ctl) : "memory");
#else
    (void)delta_ns;
#endif
}

uint32_t hal_cpu_id(void)
{
#if defined(__aarch64__)
    uint64_t mpidr = 0ULL;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFFULL);
#else
    return 0U;
#endif
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    (void)target_cpu;
    (void)ipi_type;
}
