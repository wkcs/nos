/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/mm.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_region_encoding_and_srd(void)
{
    uint32_t rbar = 0, rasr = 0;

    /* Reject < 32B or unaligned base */
    assert(nos_mpu_encode_region(5, 0x20001000U, 16U, 0U, &rbar, &rasr) == -EINVAL);
    assert(nos_mpu_encode_region(5, 0x20001010U, 32U, 0U, &rbar, &rasr) == -EINVAL);

    /* 32-byte Stack Guard (No-Access, XN=1) at 0x20002000 in Region 5 */
    assert(nos_mpu_encode_region(NOS_MPU_REGION_STACK_GUARD, 0x20002000U, 32U, 0U, &rbar, &rasr) == 0);
    assert((rbar & 0xFFFFFFE0U) == 0x20002000U);
    assert((rbar & 0x1FU) == ((1U << 4) | NOS_MPU_REGION_STACK_GUARD));
    assert((rasr & 1U) == 1U);                        /* ENABLE = 1 */
    assert(((rasr >> 1) & 0x1FU) == 4U);              /* SIZE = log2(32) - 1 = 4 */
    assert(((rasr >> 24) & 0x7U) == 0U);              /* AP = 000 (No Access) */
    assert(((rasr >> 28) & 0x1U) == 1U);              /* XN = 1 */

    /* 1MB Flash Region 1: Read-Only + Executable */
    assert(nos_mpu_encode_region(NOS_MPU_REGION_FLASH, 0x08000000U, 1024U * 1024U,
                                 HAL_VM_PROT_READ | HAL_VM_PROT_EXEC, &rbar, &rasr) == 0);
    assert(((rasr >> 1) & 0x1FU) == 19U);             /* log2(1MB) - 1 = 19 */
    assert(((rasr >> 28) & 0x1U) == 0U);              /* XN = 0 (Executable) */
    assert(((rasr >> 24) & 0x7U) == 6U);              /* AP = 110 (RO) */

    /* Non-power-of-two size using 8 Sub-Region Disable (SRD) bits:
     * 3KB (3072B) starting at 0x20004000 inside a 4KB container (8 * 512B subregions):
     * subregions 0..5 enabled (0), subregions 6..7 disabled (1 -> SRD = 0xC0).
     */
    assert(nos_mpu_encode_region(NOS_MPU_REGION_DYN0, 0x20004000U, 3072U,
                                 HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER,
                                 &rbar, &rasr) == 0);
    assert((rbar & 0xFFFFFFE0U) == 0x20004000U);
    assert(((rasr >> 1) & 0x1FU) == 11U);             /* 4KB container: log2(4096)-1 = 11 */
    assert(((rasr >> 8) & 0xFFU) == 0xC0U);           /* Subregions 6 & 7 disabled */
}

static void test_vm_space_slots_and_switch(void)
{
    struct nos_vm_space space;
    uint32_t hw_rbar[8];
    uint32_t hw_rasr[8];

    assert(nos_mpu_space_init(&space) == 0);
    assert(nos_mpu_set_stack_guard(&space, 0x20003000U) == 0);
    assert(nos_mpu_map(&space, 0x20008000U, 0x20008000U, 4096U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER) == 0);
    assert(nos_mpu_map(&space, 0x40010000U, 0x40010000U, 1024U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV) == 0);
    /* 3rd dynamic mapping must fail with -ENOSPC since 2 dynamic slots exist */
    assert(nos_mpu_map(&space, 0x2000C000U, 0x2000C000U, 1024U,
                       HAL_VM_PROT_READ) == -ENOSPC);

    memset(hw_rbar, 0, sizeof(hw_rbar));
    memset(hw_rasr, 0, sizeof(hw_rasr));
    nos_mpu_export_dynamic_regs(&space, &hw_rbar[5], &hw_rasr[5]);
    assert((hw_rbar[5] & 0xFFFFFFE0U) == 0x20003000U);
    assert((hw_rbar[6] & 0xFFFFFFE0U) == 0x20008000U);
    assert((hw_rbar[7] & 0xFFFFFFE0U) == 0x40010000U);

    assert(nos_mpu_unmap(&space, 0x20008000U, 4096U) == 0);
    nos_mpu_export_dynamic_regs(&space, &hw_rbar[5], &hw_rasr[5]);
    assert((hw_rasr[6] & 1U) == 0U); /* Slot 0 disabled after unmap */
    assert((hw_rasr[7] & 1U) == 1U); /* Slot 1 still active */
}

static void test_fault_classification(void)
{
    uintptr_t guard_base = 0x20004000U;

    /* 1. Address inside 32B stack guard -> STACK_OVERFLOW */
    assert(nos_mpu_classify_fault(0x82U /* MMARVALID | DACCVIOL */,
                                  true, guard_base + 16U, guard_base) == NOS_MPU_FAULT_STACK_OVERFLOW);
    /* 2. Hardware stacking error MSTKERR (bit 4 of MMFSR) -> STACK_OVERFLOW */
    assert(nos_mpu_classify_fault(0x10U /* MSTKERR */,
                                  false, 0U, guard_base) == NOS_MPU_FAULT_STACK_OVERFLOW);
    /* 3. NULL pointer dereference (< 64B) -> NULL_PTR */
    assert(nos_mpu_classify_fault(0x82U, true, 0x00000008U, guard_base) == NOS_MPU_FAULT_NULL_PTR);
    /* 4. Instruction fetch violation (IACCVIOL = bit 0) -> EXEC_XN */
    assert(nos_mpu_classify_fault(0x01U, false, 0x20005000U, guard_base) == NOS_MPU_FAULT_EXEC_XN);
    /* 5. General data access violation -> ACCESS_VIOLATION */
    assert(nos_mpu_classify_fault(0x82U, true, 0x08001000U, guard_base) == NOS_MPU_FAULT_ACCESS_VIOLATION);
}

int main(void)
{
    test_region_encoding_and_srd();
    test_vm_space_slots_and_switch();
    test_fault_classification();
    puts("MPU subsystem checks passed");
    return 0;
}
