/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM PSCI 0.2/1.0 调用封装（支持 HVC 与 SMC 通道切换）
 */

#include <asm/psci.h>

static bool g_psci_use_smc;

void arm64_psci_set_conduit_smc(bool use_smc)
{
    g_psci_use_smc = use_smc;
}

static int64_t psci_call(uint64_t fid, uint64_t a0, uint64_t a1, uint64_t a2)
{
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a0;
    register uint64_t x2 __asm__("x2") = a1;
    register uint64_t x3 __asm__("x3") = a2;

    if (g_psci_use_smc) {
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :: "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                            "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                            "memory", "cc");
    } else {
        __asm__ volatile("hvc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :: "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                            "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                            "memory", "cc");
    }
    return (int64_t)x0;
#else
    (void)fid;
    (void)a0;
    (void)a1;
    (void)a2;
    return PSCI_RET_NOT_SUPPORTED;
#endif
}

int32_t psci_get_version(void)
{
    return (int32_t)psci_call(PSCI_FN_VERSION, 0ULL, 0ULL, 0ULL);
}

int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_phys, uint64_t context_id)
{
    int64_t ret = psci_call(PSCI_FN64_CPU_ON, target_mpidr,
                            (uint64_t)entry_phys, context_id);
    return (int)ret;
}

void psci_system_reset(void)
{
    (void)psci_call(PSCI_FN_SYSTEM_RESET, 0ULL, 0ULL, 0ULL);
    while (1) {
#if defined(__aarch64__)
        __asm__ volatile("wfi" ::: "memory");
#endif
    }
}

void psci_system_off(void)
{
    (void)psci_call(PSCI_FN_SYSTEM_OFF, 0ULL, 0ULL, 0ULL);
    while (1) {
#if defined(__aarch64__)
        __asm__ volatile("wfi" ::: "memory");
#endif
    }
}
