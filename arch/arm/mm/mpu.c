/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARMv7-M (Cortex-M3/M4) Hardware Memory Protection Unit (PMSAv7) Driver
 */

#define pr_fmt(fmt) "[MPU]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/mm.h>
#include <nos/hal_arch.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>

/* Architectural ARMv7-M System Control Space (SCS) & MPU register map */
struct armv7m_mpu_regs {
    volatile uint32_t type;
    volatile uint32_t ctrl;
    volatile uint32_t rnr;
    volatile uint32_t rbar;
    volatile uint32_t rasr;
    volatile uint32_t rbar_a1;
    volatile uint32_t rasr_a1;
    volatile uint32_t rbar_a2;
    volatile uint32_t rasr_a2;
    volatile uint32_t rbar_a3;
    volatile uint32_t rasr_a3;
};

#define ARMV7M_MPU_BASE         ((struct armv7m_mpu_regs *)0xE000ED90UL)
#define ARMV7M_SCB_VTOR         (*(volatile uint32_t *)0xE000ED08UL)
#define ARMV7M_SCB_SHCSR        (*(volatile uint32_t *)0xE000ED24UL)
#define ARMV7M_SCB_CFSR         (*(volatile uint32_t *)0xE000ED28UL)
#define ARMV7M_SCB_MMFAR        (*(volatile uint32_t *)0xE000ED34UL)

#define ARMV7M_SHCSR_MEMFAULTENA (1UL << 16)
#define ARMV7M_MPU_CTRL_ENABLE   (1UL << 0)
#define ARMV7M_MPU_CTRL_PRIVDEF  (1UL << 2)

static bool g_hw_mpu_active;

static inline void armv7m_dsb_isb(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dsb\n\tisb" ::: "memory");
#endif
}

static void mpu_write_static_region(uint32_t region,
                                    uintptr_t base,
                                    size_t size,
                                    uint32_t prot)
{
    uint32_t rbar = 0U;
    uint32_t rasr = 0U;

    if (nos_mpu_encode_region(region, base, size, prot, &rbar, &rasr) == 0) {
        ARMV7M_MPU_BASE->rbar = rbar;
        ARMV7M_MPU_BASE->rasr = rasr;
    }
}

void arch_mpu_init(void)
{
#ifdef CONFIG_MPU
    uint32_t dregions = (ARMV7M_MPU_BASE->type >> 8U) & 0xFFU;
    uint32_t i;

    if (dregions < NOS_MPU_TOTAL_REGIONS) {
        pr_info("hardware MPU not present (dregions=%u)\n", dregions);
        g_hw_mpu_active = false;
        return;
    }

    ARMV7M_MPU_BASE->ctrl = 0U;
    armv7m_dsb_isb();

    for (i = 0U; i < dregions; i++) {
        ARMV7M_MPU_BASE->rnr = i;
        ARMV7M_MPU_BASE->rbar = 0U;
        ARMV7M_MPU_BASE->rasr = 0U;
    }

    /* Region 0: NULL pointer trap (0x00000000, 64B, No-Access) if vectors relocated */
    if (ARMV7M_SCB_VTOR != 0U) {
        mpu_write_static_region(NOS_MPU_REGION_NULL_TRAP, 0x00000000UL, 64U, 0U);
    }

    /* Region 1: Internal Flash (0x08000000, 1MB, Read-Only + Executable) */
    mpu_write_static_region(NOS_MPU_REGION_FLASH, 0x08000000UL, 1024U * 1024U,
                            HAL_VM_PROT_READ | HAL_VM_PROT_EXEC | HAL_VM_PROT_USER);

    /* Region 2: Internal SRAM (0x20000000, 128KB, Read-Write + Execute-Never) */
    mpu_write_static_region(NOS_MPU_REGION_SRAM, 0x20000000UL, 128U * 1024U,
                            HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER);

    /* Region 3: Peripherals (0x40000000, 512MB, Device Read-Write + Execute-Never) */
    mpu_write_static_region(NOS_MPU_REGION_PERIPH, 0x40000000UL, 512U * 1024U * 1024U,
                            HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV);

    /* Region 4: External FSMC SRAM (0x60000000, 256MB, Read-Write + Execute-Never) */
    mpu_write_static_region(NOS_MPU_REGION_EXT_SRAM, 0x60000000UL, 256U * 1024U * 1024U,
                            HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER);

    /* Enable MemManage Fault Handler in SCB->SHCSR */
    ARMV7M_SCB_SHCSR |= ARMV7M_SHCSR_MEMFAULTENA;

    /* Enable MPU with Privileged Background Region (PRIVDEFENA) */
    ARMV7M_MPU_BASE->ctrl = ARMV7M_MPU_CTRL_ENABLE | ARMV7M_MPU_CTRL_PRIVDEF;
    armv7m_dsb_isb();
    g_hw_mpu_active = true;
    pr_info("ARMv7-M MPU enabled (%u regions, stack guard + XN active)\n", dregions);
#else
    g_hw_mpu_active = false;
#endif
}

void arch_mpu_switch(const struct nos_vm_space *space)
{
    uint32_t rbar[3];
    uint32_t rasr[3];

    if (!g_hw_mpu_active) {
        return;
    }

    nos_mpu_export_dynamic_regs(space, rbar, rasr);

    /* Program Regions 5, 6, 7 in a single burst using RBAR/RASR + Alias 1 & 2 */
    ARMV7M_MPU_BASE->rbar    = rbar[0];
    ARMV7M_MPU_BASE->rasr    = rasr[0];
    ARMV7M_MPU_BASE->rbar_a1 = rbar[1];
    ARMV7M_MPU_BASE->rasr_a1 = rasr[1];
    ARMV7M_MPU_BASE->rbar_a2 = rbar[2];
    ARMV7M_MPU_BASE->rasr_a2 = rasr[2];
    armv7m_dsb_isb();
}

void arch_mpu_handle_fault(uintptr_t sp)
{
    uint32_t cfsr = ARMV7M_SCB_CFSR;
    uint8_t mmfsr = (uint8_t)(cfsr & 0xFFU);
    bool mmar_valid = (mmfsr & 0x80U) != 0U;
    uintptr_t mmfar = mmar_valid ? (uintptr_t)ARMV7M_SCB_MMFAR : 0U;
    struct task_struct *tsk = current;
    uintptr_t guard_base = (tsk != NULL) ? tsk->vm_space.stack_guard_base : 0U;
    enum nos_mpu_fault_type ftype = nos_mpu_classify_fault(mmfsr, mmar_valid, mmfar, guard_base);
    const char *task_name = (tsk != NULL && tsk->name != NULL) ? tsk->name : "kernel";
    uint32_t task_pid = (tsk != NULL) ? (uint32_t)tsk->pid : 0U;

    (void)sp;
    switch (ftype) {
    case NOS_MPU_FAULT_STACK_OVERFLOW:
        pr_fatal("[MPU FAULT] Task '%s' (pid=%u) STACK OVERFLOW at 0x%08lx (guard=0x%08lx)\n",
                 task_name, task_pid, (unsigned long)mmfar, (unsigned long)guard_base);
        break;
    case NOS_MPU_FAULT_NULL_PTR:
        pr_fatal("[MPU FAULT] Task '%s' (pid=%u) NULL pointer dereference at 0x%08lx\n",
                 task_name, task_pid, (unsigned long)mmfar);
        break;
    case NOS_MPU_FAULT_EXEC_XN:
        pr_fatal("[MPU FAULT] Task '%s' (pid=%u) Execute-Never (XN) violation (CFSR=0x%08lx)\n",
                 task_name, task_pid, (unsigned long)cfsr);
        break;
    default:
        pr_fatal("[MPU FAULT] Task '%s' (pid=%u) memory access violation at 0x%08lx (CFSR=0x%08lx)\n",
                 task_name, task_pid, (unsigned long)mmfar, (unsigned long)cfsr);
        break;
    }
}
