/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARMv7-M (PMSAv7) MPU Region Encoder, Dynamic Slot Manager & Fault Classifier
 */

#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <string.h>

/* PMSAv7 RBAR / RASR bit definitions */
#define NOS_MPU_RBAR_ADDR_MASK      0xFFFFFFE0U
#define NOS_MPU_RBAR_VALID          (1U << 4)
#define NOS_MPU_RBAR_REGION_MASK    0x0000000FU

#define NOS_MPU_RASR_XN_SHIFT       28U
#define NOS_MPU_RASR_AP_SHIFT       24U
#define NOS_MPU_RASR_TEX_SHIFT      19U
#define NOS_MPU_RASR_S_SHIFT        18U
#define NOS_MPU_RASR_C_SHIFT        17U
#define NOS_MPU_RASR_B_SHIFT        16U
#define NOS_MPU_RASR_SRD_SHIFT      8U
#define NOS_MPU_RASR_SIZE_SHIFT     1U
#define NOS_MPU_RASR_ENABLE         1U

/* Cortex-M MMFSR bit masks */
#define NOS_MMFSR_IACCVIOL          (1U << 0)
#define NOS_MMFSR_DACCVIOL          (1U << 1)
#define NOS_MMFSR_MUNSTKERR         (1U << 3)
#define NOS_MMFSR_MSTKERR           (1U << 4)
#define NOS_MMFSR_MMARVALID         (1U << 7)

static bool is_power_of_two(size_t x)
{
    return x != 0U && (x & (x - 1U)) == 0U;
}

static uint32_t calc_log2_u32(uint32_t val)
{
    return 31U - (uint32_t)__builtin_clz(val);
}

int nos_mpu_encode_region(uint32_t region_num,
                          uintptr_t base,
                          size_t size,
                          uint32_t prot_flags,
                          uint32_t *out_rbar,
                          uint32_t *out_rasr)
{
    uint32_t container_size;
    uint32_t size_field;
    uint32_t srd_mask = 0U;
    uint32_t ap;
    uint32_t xn;
    uint32_t mem_attrs;
    uintptr_t container_base;

    if (out_rbar == NULL || out_rasr == NULL || region_num >= NOS_MPU_TOTAL_REGIONS) {
        return -EINVAL;
    }
    if (size < 32U || size > 0x80000000UL || base > 0xFFFFFFFFUL) {
        return -EINVAL;
    }

    if (is_power_of_two(size)) {
        container_size = (uint32_t)size;
        if ((base & (container_size - 1U)) != 0U) {
            return -EINVAL;
        }
        container_base = base;
    } else {
        /* Attempt 8-subregion SRD encoding inside the next power-of-two container >= 256B */
        uint32_t log2_ceil = calc_log2_u32((uint32_t)size - 1U) + 1U;
        uint32_t sub_size;
        uint32_t start_sub;
        uint32_t num_subs;
        uint32_t i;

        if (log2_ceil < 8U || log2_ceil > 31U) {
            return -EINVAL;
        }
        container_size = 1U << log2_ceil;
        sub_size = container_size >> 3U;
        if ((size % sub_size) != 0U || (base % sub_size) != 0U) {
            return -EINVAL;
        }
        container_base = base & ~((uintptr_t)container_size - 1U);
        start_sub = (uint32_t)((base - container_base) / sub_size);
        num_subs = (uint32_t)(size / sub_size);
        if (start_sub + num_subs > 8U) {
            return -EINVAL;
        }
        srd_mask = 0xFFU;
        for (i = start_sub; i < start_sub + num_subs; i++) {
            srd_mask &= ~(1U << i);
        }
    }

    size_field = calc_log2_u32(container_size) - 1U;

    /* Encode Access Permissions (AP[2:0]) */
    if ((prot_flags & (HAL_VM_PROT_READ | HAL_VM_PROT_WRITE)) == 0U) {
        ap = 0U; /* 000: No access (Priv & Unpriv) */
    } else if (prot_flags & HAL_VM_PROT_WRITE) {
        ap = (prot_flags & HAL_VM_PROT_USER) ? 3U : 1U; /* 011: Full RW, 001: Priv RW */
    } else {
        ap = 6U; /* 110: Privileged & Unprivileged Read-Only */
    }

    xn = (prot_flags & HAL_VM_PROT_EXEC) ? 0U : 1U;

    /* Memory attributes: Device vs Flash (WT) vs SRAM (WBWA) */
    if (prot_flags & HAL_VM_PROT_DEV) {
        mem_attrs = (1U << NOS_MPU_RASR_B_SHIFT) | (1U << NOS_MPU_RASR_S_SHIFT);
    } else if (prot_flags & HAL_VM_PROT_EXEC) {
        mem_attrs = (1U << NOS_MPU_RASR_C_SHIFT); /* Normal Write-Through for Flash */
    } else {
        mem_attrs = (1U << NOS_MPU_RASR_C_SHIFT) |
                    (1U << NOS_MPU_RASR_B_SHIFT) |
                    (1U << NOS_MPU_RASR_S_SHIFT); /* Normal Write-Back Shareable for SRAM */
    }

    *out_rbar = ((uint32_t)container_base & NOS_MPU_RBAR_ADDR_MASK) |
                NOS_MPU_RBAR_VALID |
                (region_num & NOS_MPU_RBAR_REGION_MASK);
    *out_rasr = (xn << NOS_MPU_RASR_XN_SHIFT) |
                (ap << NOS_MPU_RASR_AP_SHIFT) |
                mem_attrs |
                (srd_mask << NOS_MPU_RASR_SRD_SHIFT) |
                (size_field << NOS_MPU_RASR_SIZE_SHIFT) |
                NOS_MPU_RASR_ENABLE;
    return 0;
}

int nos_mpu_space_init(struct nos_vm_space *space)
{
    uint32_t i;

    if (space == NULL) {
        return -EINVAL;
    }
    space->mpu_region_mask = 0U;
    space->stack_guard_base = 0U;
    space->guard_rbar = NOS_MPU_RBAR_VALID | NOS_MPU_REGION_STACK_GUARD;
    space->guard_rasr = 0U;
    for (i = 0U; i < NOS_MPU_DYNAMIC_SLOTS; i++) {
        uint32_t hw_region = NOS_MPU_REGION_DYN0 + i;
        space->mpu_slots[i].base = 0U;
        space->mpu_slots[i].size = 0U;
        space->mpu_slots[i].prot = 0U;
        space->mpu_slots[i].rbar = NOS_MPU_RBAR_VALID | hw_region;
        space->mpu_slots[i].rasr = 0U;
        space->mpu_slots[i].active = false;
    }
    return 0;
}

void nos_mpu_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL) {
        (void)nos_mpu_space_init(space);
    }
}

int nos_mpu_set_stack_guard(struct nos_vm_space *space, uintptr_t guard_base)
{
    uint32_t rbar = 0U;
    uint32_t rasr = 0U;
    int rc;

    if (space == NULL) {
        return -EINVAL;
    }
    if (guard_base == 0U) {
        space->stack_guard_base = 0U;
        space->guard_rbar = NOS_MPU_RBAR_VALID | NOS_MPU_REGION_STACK_GUARD;
        space->guard_rasr = 0U;
        space->mpu_region_mask &= ~(1U << NOS_MPU_REGION_STACK_GUARD);
        return 0;
    }

    rc = nos_mpu_encode_region(NOS_MPU_REGION_STACK_GUARD,
                               guard_base,
                               NOS_MPU_GUARD_SIZE,
                               0U /* No-Access */,
                               &rbar,
                               &rasr);
    if (rc < 0) {
        return rc;
    }
    space->stack_guard_base = guard_base;
    space->guard_rbar = rbar;
    space->guard_rasr = rasr;
    space->mpu_region_mask |= (1U << NOS_MPU_REGION_STACK_GUARD);
    return 0;
}

int nos_mpu_map(struct nos_vm_space *space,
                uintptr_t vaddr,
                uintptr_t paddr,
                size_t size,
                uint32_t prot_flags)
{
    uint32_t i;
    uint32_t free_slot = NOS_MPU_DYNAMIC_SLOTS;
    uint32_t hw_region;
    uint32_t rbar = 0U;
    uint32_t rasr = 0U;
    int rc;

    if (space == NULL || vaddr != paddr || size == 0U) {
        return -EINVAL;
    }

    for (i = 0U; i < NOS_MPU_DYNAMIC_SLOTS; i++) {
        if (space->mpu_slots[i].active && space->mpu_slots[i].base == vaddr) {
            free_slot = i;
            break;
        }
        if (!space->mpu_slots[i].active && free_slot == NOS_MPU_DYNAMIC_SLOTS) {
            free_slot = i;
        }
    }
    if (free_slot >= NOS_MPU_DYNAMIC_SLOTS) {
        return -ENOSPC;
    }

    hw_region = NOS_MPU_REGION_DYN0 + free_slot;
    rc = nos_mpu_encode_region(hw_region, vaddr, size, prot_flags, &rbar, &rasr);
    if (rc < 0) {
        return rc;
    }

    space->mpu_slots[free_slot].base = vaddr;
    space->mpu_slots[free_slot].size = size;
    space->mpu_slots[free_slot].prot = prot_flags;
    space->mpu_slots[free_slot].rbar = rbar;
    space->mpu_slots[free_slot].rasr = rasr;
    space->mpu_slots[free_slot].active = true;
    space->mpu_region_mask |= (1U << hw_region);
    return 0;
}

int nos_mpu_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    uint32_t i;

    if (space == NULL || size == 0U) {
        return -EINVAL;
    }
    for (i = 0U; i < NOS_MPU_DYNAMIC_SLOTS; i++) {
        if (space->mpu_slots[i].active &&
            space->mpu_slots[i].base == vaddr &&
            space->mpu_slots[i].size == size) {
            uint32_t hw_region = NOS_MPU_REGION_DYN0 + i;
            space->mpu_slots[i].active = false;
            space->mpu_slots[i].base = 0U;
            space->mpu_slots[i].size = 0U;
            space->mpu_slots[i].prot = 0U;
            space->mpu_slots[i].rbar = NOS_MPU_RBAR_VALID | hw_region;
            space->mpu_slots[i].rasr = 0U;
            space->mpu_region_mask &= ~(1U << hw_region);
            return 0;
        }
    }
    return -ENOENT;
}

void nos_mpu_export_dynamic_regs(const struct nos_vm_space *space,
                                 uint32_t out_rbar[3],
                                 uint32_t out_rasr[3])
{
    uint32_t i;

    if (out_rbar == NULL || out_rasr == NULL) {
        return;
    }
    if (space == NULL) {
        out_rbar[0] = NOS_MPU_RBAR_VALID | NOS_MPU_REGION_STACK_GUARD;
        out_rasr[0] = 0U;
        for (i = 0U; i < NOS_MPU_DYNAMIC_SLOTS; i++) {
            out_rbar[1U + i] = NOS_MPU_RBAR_VALID | (NOS_MPU_REGION_DYN0 + i);
            out_rasr[1U + i] = 0U;
        }
        return;
    }

    out_rbar[0] = space->guard_rbar ? space->guard_rbar
                                    : (NOS_MPU_RBAR_VALID | NOS_MPU_REGION_STACK_GUARD);
    out_rasr[0] = space->guard_rasr;
    for (i = 0U; i < NOS_MPU_DYNAMIC_SLOTS; i++) {
        out_rbar[1U + i] = space->mpu_slots[i].active
                               ? space->mpu_slots[i].rbar
                               : (NOS_MPU_RBAR_VALID | (NOS_MPU_REGION_DYN0 + i));
        out_rasr[1U + i] = space->mpu_slots[i].active ? space->mpu_slots[i].rasr : 0U;
    }
}

enum nos_mpu_fault_type nos_mpu_classify_fault(uint8_t mmfsr,
                                               bool mmar_valid,
                                               uintptr_t mmfar,
                                               uintptr_t stack_guard_base)
{
    if (mmfsr & NOS_MMFSR_MSTKERR) {
        return NOS_MPU_FAULT_STACK_OVERFLOW;
    }
    if (mmar_valid) {
        if (stack_guard_base != 0U &&
            mmfar >= stack_guard_base &&
            mmfar < stack_guard_base + NOS_MPU_GUARD_SIZE) {
            return NOS_MPU_FAULT_STACK_OVERFLOW;
        }
        if (mmfar < 64U) {
            return NOS_MPU_FAULT_NULL_PTR;
        }
    }
    if (mmfsr & NOS_MMFSR_IACCVIOL) {
        return NOS_MPU_FAULT_EXEC_XN;
    }
    if (mmfsr != 0U) {
        return NOS_MPU_FAULT_ACCESS_VIOLATION;
    }
    return NOS_MPU_FAULT_NONE;
}
