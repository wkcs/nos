/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Multi-Architecture Hardware Abstraction Layer (HAL) Contract
 * Implemented modularly by arch/{arm,arm64,riscv,x86}/
 */

#ifndef __NOS_HAL_ARCH_H__
#define __NOS_HAL_ARCH_H__

#include <uapi/nos/types.h>

struct nos_arch_context;
struct nos_vm_space;

/* 1. CPU & Trap Initialization (entry.S / trap.c) */
void hal_arch_early_init(void);
void hal_arch_trap_init(void);
void hal_arch_context_init(struct nos_arch_context *ctx,
                           uintptr_t entry,
                           uintptr_t stack_top,
                           uintptr_t arg0,
                           bool is_user);
void hal_arch_context_switch(struct nos_arch_context *prev,
                             struct nos_arch_context *next);

/* 2. Interrupt Controller Interface (irq.c) */
uintptr_t hal_irq_save(void);
void hal_irq_restore(uintptr_t flags);
void hal_irq_enable(uint32_t irq);
void hal_irq_disable(uint32_t irq);
void hal_irq_ack(uint32_t irq);

/* 3. Address Space Interface: MMU Page Tables or MPU/PMP Regions (mmu.c / mpu.c) */
#define HAL_VM_PROT_READ    (1U << 0)
#define HAL_VM_PROT_WRITE   (1U << 1)
#define HAL_VM_PROT_EXEC    (1U << 2)
#define HAL_VM_PROT_USER    (1U << 3)
#define HAL_VM_PROT_DEV     (1U << 4)

int hal_vm_space_init(struct nos_vm_space *space);
void hal_vm_space_destroy(struct nos_vm_space *space);
int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags);
int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size);
void hal_vm_switch(struct nos_vm_space *next_space);

/* 4. Timer Interface (timer.c) */
void hal_timer_init(uint32_t hz);
uint64_t hal_timer_monotonic_ns(void);
void hal_timer_program_oneshot_ns(uint64_t delta_ns);

/* 5. SMP & Per-CPU Interface (smp.c) */
uint32_t hal_cpu_id(void);
void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type);

#endif /* __NOS_HAL_ARCH_H__ */
