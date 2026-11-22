/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 统一多架构 HAL 契约实现（EL1/EL0、GICv3/v2、Generic Timer、Hardware MMU）
 */

#include <nos/hal_arch.h>
#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <asm/init.h>
#include <asm/irq.h>
#include <asm/gic.h>
#include <asm/mmu.h>
#include <asm/percpu.h>
#include <asm/psci.h>
#include <asm/switch.h>
#include <asm/sysreg.h>
#include <kernel/mm.h>
#include <string.h>

extern volatile uint32_t interrupt_nest;
extern uint64_t arm64_timer_monotonic_ns(void);
extern void arm64_timer_init_hz(uint32_t hz);
extern void arm64_timer_program_oneshot_ns(uint64_t delta_ns);

struct nos_arch_context {
    struct arm64_switch_frame frame;
    uint64_t                  sp;
    uint64_t                  pc;
    uint64_t                  pstate;
};

void hal_arch_early_init(void)
{
    arch_early_init();
}

void hal_arch_trap_init(void)
{
    /* GIC 具体基址由 board_init() 解析 DTB/平台定义后调用 arm64_gic_init() 完成 */
}

void hal_arch_context_init(struct nos_arch_context *ctx,
                           uintptr_t entry,
                           uintptr_t stack_top,
                           uintptr_t arg0,
                           bool is_user)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->frame.x19 = (uint64_t)arg0;
    ctx->frame.x30 = (uint64_t)entry;
    ctx->sp = (uint64_t)(stack_top & ~0xFULL);
    ctx->pc = (uint64_t)entry;
    ctx->pstate = is_user ? PSR_MODE_EL0t : PSR_MODE_EL1h;
}

void hal_arch_context_switch(struct nos_arch_context *prev,
                             struct nos_arch_context *next)
{
    if (prev == NULL || next == NULL) {
        return;
    }
    __arm64_cpu_switch_to((addr_t)&prev->sp, (addr_t)&next->sp);
}

uintptr_t hal_irq_save(void)
{
    return (uintptr_t)asm_disable_irq_save();
}

void hal_irq_restore(uintptr_t flags)
{
    asm_enable_irq_save((addr_t)flags);
}

void hal_irq_enable(uint32_t irq)
{
    arm64_gic_enable_irq(irq);
}

void hal_irq_disable(uint32_t irq)
{
    arm64_gic_disable_irq(irq);
}

void hal_irq_ack(uint32_t irq)
{
    arm64_gic_eoi(irq);
}

bool hal_in_irq(void)
{
    return (interrupt_nest > 0U);
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    addr_t root_va;
    uint16_t asid;

    if (space == NULL) {
        return -EINVAL;
    }
    memset(space, 0, sizeof(*space));
    spin_lock_init(&space->lock);
    INIT_LIST_HEAD(&space->vma_list);
    space->mmap_base = 0x00100000UL;
    space->mmap_limit = 0x3FFFFFF000UL;

    root_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (root_va == 0U) {
        return -ENOMEM;
    }
    asid = arm64_asid_alloc(NULL, NULL);
    space->root_pt_phys = arm64_virt_to_phys((uintptr_t)root_va);
    space->asid = asid;
    space->initialized = true;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    struct list_head *pos;
    struct list_head *n;

    if (space == NULL || !space->initialized) {
        return;
    }
    list_for_each_safe(pos, n, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        list_del(&vma->list);
        kfree(vma);
    }
    if (space->root_pt_phys != 0U) {
        arm64_pt_destroy(space->root_pt_phys, CONFIG_ARM64_VA_BITS, NULL);
        space->root_pt_phys = 0U;
    }
    space->initialized = false;
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    int rc = arm64_pt_map_range(space->root_pt_phys,
                                CONFIG_ARM64_VA_BITS,
                                vaddr, paddr, size,
                                prot_flags, true, NULL);
    return rc;
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    if (space == NULL || !space->initialized || space->root_pt_phys == 0U) {
        return -EINVAL;
    }
    return arm64_pt_unmap_range(space->root_pt_phys,
                                CONFIG_ARM64_VA_BITS,
                                vaddr, size, true, NULL);
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    if (next_space == NULL || !next_space->initialized || next_space->root_pt_phys == 0U) {
        return;
    }
#if defined(__aarch64__)
    uint64_t ttbr = ((uint64_t)next_space->asid << 48) |
                    (uint64_t)(next_space->root_pt_phys & ARM64_PTE_ADDR_MASK);
    __asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(ttbr) : "memory");
#endif
}

void hal_timer_init(uint32_t hz)
{
    arm64_timer_init_hz(hz);
}

uint64_t hal_timer_monotonic_ns(void)
{
    return arm64_timer_monotonic_ns();
}

void hal_timer_program_oneshot_ns(uint64_t delta_ns)
{
    arm64_timer_program_oneshot_ns(delta_ns);
}

uint32_t hal_cpu_id(void)
{
    return arm64_this_percpu()->cpu_id;
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    arm64_gic_send_sgi(target_cpu, ipi_type & 0xFU);
}
