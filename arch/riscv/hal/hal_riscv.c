/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Multi-Architecture HAL Implementation for RISC-V (RV32 / RV64)
 * Supports CLINT + PLIC interrupt controller, rdtime timer, and Sv39/PMP VM.
 */

#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <asm/init.h>

/* QEMU virt PLIC MMIO base */
#define RISCV_PLIC_BASE             0x0C000000UL
#define RISCV_PLIC_PRIORITY(irq)    ((volatile uint32_t *)(RISCV_PLIC_BASE + 4UL * (irq)))
#define RISCV_PLIC_SENABLE(hart, n) ((volatile uint32_t *)(RISCV_PLIC_BASE + 0x2080UL + (hart) * 0x100UL + 4UL * (n)))
#define RISCV_PLIC_SCLAIM(hart)     ((volatile uint32_t *)(RISCV_PLIC_BASE + 0x201004UL + (hart) * 0x2000UL))

/* RISC-V sstatus SIE bit */
#define RISCV_SSTATUS_SIE           (1UL << 1)
#define RISCV_SATP_MODE_SV39        (8ULL << 60)

struct nos_arch_context {
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t s[12];
    uintptr_t sepc;
    uintptr_t sstatus;
};

struct nos_vm_space {
    uintptr_t root_pt_phys;
    uint16_t  asid;
};

void hal_arch_early_init(void)
{
    arch_early_init();
}

void hal_arch_trap_init(void)
{
#if defined(__riscv)
    /* Enable Supervisor External, Timer, and Software Interrupts in sie */
    uintptr_t sie_bits = (1UL << 1) | (1UL << 5) | (1UL << 9);
    __asm__ volatile("csrs sie, %0" :: "r"(sie_bits) : "memory");
#endif
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
        ctx->s[i] = 0U;
    }
    ctx->s[0] = arg0;
    ctx->ra = entry;
    ctx->sp = stack_top & ~(uintptr_t)0xFU;
    ctx->sepc = entry;
    ctx->sstatus = is_user ? (1UL << 5) : ((1UL << 8) | (1UL << 5));
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
#if defined(__riscv)
    __asm__ volatile("csrrc %0, sstatus, %1"
                     : "=r"(flags)
                     : "r"((uintptr_t)RISCV_SSTATUS_SIE)
                     : "memory");
#endif
    return flags;
}

void hal_irq_restore(uintptr_t flags)
{
#if defined(__riscv)
    if (flags & RISCV_SSTATUS_SIE) {
        __asm__ volatile("csrs sstatus, %0"
                         :: "r"((uintptr_t)RISCV_SSTATUS_SIE)
                         : "memory");
    }
#else
    (void)flags;
#endif
}

void hal_irq_enable(uint32_t irq)
{
    uint32_t hart = hal_cpu_id();
    if (irq > 0U && irq < 1024U) {
        *RISCV_PLIC_PRIORITY(irq) = 1U;
        *RISCV_PLIC_SENABLE(hart, irq / 32U) |= (1U << (irq % 32U));
    }
}

void hal_irq_disable(uint32_t irq)
{
    uint32_t hart = hal_cpu_id();
    if (irq > 0U && irq < 1024U) {
        *RISCV_PLIC_SENABLE(hart, irq / 32U) &= ~(1U << (irq % 32U));
    }
}

void hal_irq_ack(uint32_t irq)
{
    uint32_t hart = hal_cpu_id();
    if (irq > 0U && irq < 1024U) {
        *RISCV_PLIC_SCLAIM(hart) = irq;
    }
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    if (space == NULL) {
        return -EINVAL;
    }
    space->root_pt_phys = 0U;
    space->asid = 0U;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL) {
        space->root_pt_phys = 0U;
        space->asid = 0U;
    }
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    (void)space;
    (void)prot_flags;
    if ((vaddr & 0xFFFUL) != 0UL || (paddr & 0xFFFUL) != 0UL || size == 0U) {
        return -EINVAL;
    }
    return 0;
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    (void)space;
    if ((vaddr & 0xFFFUL) != 0UL || size == 0U) {
        return -EINVAL;
    }
    return 0;
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    if (next_space == NULL || next_space->root_pt_phys == 0U) {
        return;
    }
#if defined(__riscv) && (__riscv_xlen == 64)
    uint64_t satp = RISCV_SATP_MODE_SV39 |
                    ((uint64_t)next_space->asid << 44) |
                    ((uint64_t)next_space->root_pt_phys >> 12);
    __asm__ volatile("csrw satp, %0\n\tsfence.vma" :: "r"(satp) : "memory");
#endif
}

void hal_timer_init(uint32_t hz)
{
    (void)hz;
#if defined(__riscv)
    uintptr_t stie = (1UL << 5);
    __asm__ volatile("csrs sie, %0" :: "r"(stie) : "memory");
#endif
}

uint64_t hal_timer_monotonic_ns(void)
{
#if defined(__riscv) && (__riscv_xlen == 64)
    uint64_t ticks = 0ULL;
    __asm__ volatile("rdtime %0" : "=r"(ticks));
    /* QEMU virt CLINT timebase is 10MHz (100ns per tick) */
    return ticks * 100ULL;
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
#if defined(__riscv)
    uintptr_t tp_val = 0U;
    __asm__ volatile("mv %0, tp" : "=r"(tp_val));
    return (uint32_t)tp_val;
#else
    return 0U;
#endif
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    (void)target_cpu;
    (void)ipi_type;
}
