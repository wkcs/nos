/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Unified Dual-Mode Memory Protection (MPU) & Virtual Memory (MMU) Subsystem
 */

#ifndef __NOS_MM_SUBSYS_H__
#define __NOS_MM_SUBSYS_H__

#include <uapi/nos/types.h>
#include <nos/hal_arch.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

/* ARMv7-M PMSAv7 8-Region layout */
#define NOS_MPU_REGION_NULL_TRAP    0U
#define NOS_MPU_REGION_FLASH        1U
#define NOS_MPU_REGION_SRAM         2U
#define NOS_MPU_REGION_PERIPH       3U
#define NOS_MPU_REGION_EXT_SRAM     4U
#define NOS_MPU_REGION_STACK_GUARD  5U
#define NOS_MPU_REGION_DYN0         6U
#define NOS_MPU_REGION_DYN1         7U
#define NOS_MPU_TOTAL_REGIONS       8U

#define NOS_MPU_GUARD_SIZE          32U
#define NOS_MPU_DYNAMIC_SLOTS       2U

enum nos_mpu_fault_type {
    NOS_MPU_FAULT_NONE             = 0,
    NOS_MPU_FAULT_STACK_OVERFLOW   = 1,
    NOS_MPU_FAULT_NULL_PTR         = 2,
    NOS_MPU_FAULT_EXEC_XN          = 3,
    NOS_MPU_FAULT_ACCESS_VIOLATION = 4,
};

struct nos_mpu_slot {
    uintptr_t base;
    size_t    size;
    uint32_t  prot;
    uint32_t  rbar;
    uint32_t  rasr;
    bool      active;
};

/* Virtual Memory Area (VMA) flags for MMU mode */
#define NOS_VM_FLAG_ANON            (1U << 0)
#define NOS_VM_FLAG_DEMAND          (1U << 1)
#define NOS_VM_FLAG_COW             (1U << 2)
#define NOS_VM_FLAG_FIXED           (1U << 3)
#define NOS_VM_FLAG_OWN_PAGES       (1U << 4)

struct nos_vm_area {
    uintptr_t        vm_start;
    uintptr_t        vm_end;
    uintptr_t        phys_base;
    uint32_t         vm_prot;
    uint32_t         vm_flags;
    struct list_head list;
};

struct nos_vm_space {
    /* MPU mode state */
    uint32_t            mpu_region_mask;
    uintptr_t           stack_guard_base;
    uint32_t            guard_rbar;
    uint32_t            guard_rasr;
    struct nos_mpu_slot mpu_slots[NOS_MPU_DYNAMIC_SLOTS];

    /* MMU mode state */
    uintptr_t           root_pt_phys;
    uint16_t            asid;
    uint32_t            vma_count;
    uintptr_t           mmap_base;
    uintptr_t           mmap_limit;
    struct list_head    vma_list;
    spinlock_t          lock;
    bool                initialized;
};

/* MPU portable encoding & management API (kernel/mm/vmm_mpu.c) */
int nos_mpu_encode_region(uint32_t region_num,
                          uintptr_t base,
                          size_t size,
                          uint32_t prot_flags,
                          uint32_t *out_rbar,
                          uint32_t *out_rasr);
int nos_mpu_space_init(struct nos_vm_space *space);
void nos_mpu_space_destroy(struct nos_vm_space *space);
int nos_mpu_set_stack_guard(struct nos_vm_space *space, uintptr_t guard_base);
int nos_mpu_map(struct nos_vm_space *space,
                uintptr_t vaddr,
                uintptr_t paddr,
                size_t size,
                uint32_t prot_flags);
int nos_mpu_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size);
void nos_mpu_export_dynamic_regs(const struct nos_vm_space *space,
                                 uint32_t out_rbar[3],
                                 uint32_t out_rasr[3]);
enum nos_mpu_fault_type nos_mpu_classify_fault(uint8_t mmfsr,
                                               bool mmar_valid,
                                               uintptr_t mmfar,
                                               uintptr_t stack_guard_base);

/* ARM Cortex-M hardware MPU driver hooks (arch/arm/mm/mpu.c) */
void arch_mpu_init(void);
void arch_mpu_switch(const struct nos_vm_space *space);
void arch_mpu_handle_fault(uintptr_t sp);

/* MMU VMA & Multi-level Page Table API (kernel/mm/vmm_mmu.c) */
int nos_vmm_space_init(struct nos_vm_space *space);
void nos_vmm_space_destroy(struct nos_vm_space *space);
int nos_vmm_map_pages(struct nos_vm_space *space,
                      uintptr_t vaddr,
                      uintptr_t paddr,
                      size_t size,
                      uint32_t prot_flags);
int nos_vmm_unmap_pages(struct nos_vm_space *space,
                        uintptr_t vaddr,
                        size_t size);
int nos_vmm_translate(struct nos_vm_space *space,
                      uintptr_t vaddr,
                      uintptr_t *out_paddr,
                      uint32_t *out_prot);
int nos_vm_mmap(struct nos_vm_space *space,
                uintptr_t vaddr_hint,
                size_t size,
                uintptr_t paddr,
                uint32_t prot,
                uint32_t flags,
                uintptr_t *out_vaddr);
int nos_vm_munmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size);
int nos_vm_mprotect(struct nos_vm_space *space,
                    uintptr_t vaddr,
                    size_t size,
                    uint32_t new_prot);
struct nos_vm_area *nos_vm_find_vma(struct nos_vm_space *space, uintptr_t vaddr);
int nos_vm_clone(struct nos_vm_space *dst, struct nos_vm_space *src);
int nos_vm_handle_fault(struct nos_vm_space *space,
                        uintptr_t fault_vaddr,
                        uint32_t access_prot);
uint32_t nos_vmm_active_pt_pages(void);

#endif /* __NOS_MM_SUBSYS_H__ */
