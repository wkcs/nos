/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARMv7-M MPU 平台参数化主机单元测试
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 模拟 ARMv7-M SCB->VTOR 与 MPU 寄存器阵列的测试桩 */
static uint32_t g_test_vtor;
static uint32_t g_test_rnr;
static uint32_t g_test_rbar[8];
static uint32_t g_test_rasr[8];
static uint32_t g_test_mpu_ctrl __attribute__((unused));

#define ARMV7M_MPU_TEST_HOST 1
#define ARMV7M_TEST_SCB_VTOR g_test_vtor
#define ARMV7M_TEST_MPU_CTRL g_test_mpu_ctrl
#define ARMV7M_TEST_MPU_RNR  g_test_rnr
#define ARMV7M_TEST_MPU_RBAR (g_test_rbar[g_test_rnr & 0x7U])
#define ARMV7M_TEST_MPU_RASR (g_test_rasr[g_test_rnr & 0x7U])

static inline void test_sink(const char *fmt, ...) { (void)fmt; }
#ifndef pr_fatal
#define pr_fatal(...) test_sink(__VA_ARGS__)
#endif

struct task_struct {
    const char *name;
    uint32_t pid;
    struct {
        uintptr_t stack_guard_base;
    } vm_space;
};
#define current ((struct task_struct *)0)

#include "../../arch/arm/mm/mpu.c"

static void reset_mpu_regs(uint32_t vtor)
{
    g_test_vtor = vtor;
    g_test_rnr = 0U;
    g_test_mpu_ctrl = 0U;
    memset(g_test_rbar, 0, sizeof(g_test_rbar));
    memset(g_test_rasr, 0, sizeof(g_test_rasr));
}

int main(void)
{
#ifdef TEST_NUC472_LAYOUT
    reset_mpu_regs(0x00000000UL);
    arch_mm_init_static_regions_for_test();
    /* NUC472 的 VTOR=0 且 Flash 位于 0x00000000，禁止启用 Region 0 空指针陷阱 */
    assert((g_test_rasr[NOS_MPU_REGION_NULL_TRAP] & 1U) == 0U);
    /* Region 1: 0x00000000, 512 KiB (log2(512K)-1 = 18), 可读可执行 */
    assert((g_test_rbar[NOS_MPU_REGION_FLASH] & 0xFFFFFFE0U) == 0x00000000UL);
    assert((g_test_rasr[NOS_MPU_REGION_FLASH] & 1U) == 1U);
    assert(((g_test_rasr[NOS_MPU_REGION_FLASH] >> 1) & 0x1FU) == 18U);
    assert(((g_test_rasr[NOS_MPU_REGION_FLASH] >> 28) & 1U) == 0U);
    /* Region 2: 0x20000000, 64 KiB (log2(64K)-1 = 15), XN=1 */
    assert((g_test_rbar[NOS_MPU_REGION_SRAM] & 0xFFFFFFE0U) == 0x20000000UL);
    assert((g_test_rasr[NOS_MPU_REGION_SRAM] & 1U) == 1U);
    assert(((g_test_rasr[NOS_MPU_REGION_SRAM] >> 1) & 0x1FU) == 15U);
    assert(((g_test_rasr[NOS_MPU_REGION_SRAM] >> 28) & 1U) == 1U);
    printf("NUC472 MPU static layout test passed\n");
#else
    reset_mpu_regs(0x08000000UL);
    arch_mm_init_static_regions_for_test();
    /* STM32 默认 VTOR != 0 且 Flash != 0，Region 0 空指针陷阱生效 */
    assert((g_test_rasr[NOS_MPU_REGION_NULL_TRAP] & 1U) == 1U);
    assert((g_test_rbar[NOS_MPU_REGION_FLASH] & 0xFFFFFFE0U) == 0x08000000UL);
    assert(((g_test_rasr[NOS_MPU_REGION_FLASH] >> 1) & 0x1FU) == 19U);
    assert((g_test_rbar[NOS_MPU_REGION_SRAM] & 0xFFFFFFE0U) == 0x20000000UL);
    assert(((g_test_rasr[NOS_MPU_REGION_SRAM] >> 1) & 0x1FU) == 16U);
    printf("STM32 MPU static layout test passed\n");
#endif
    return 0;
}
