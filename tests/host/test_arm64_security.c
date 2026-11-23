/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARMv9 PAC / BTI / MTE2 安全硬化策略计算、Per-Task 密钥生成与 PTE 位编码主机单测
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nos/hal_arch.h>
#include <asm/cpufeature.h>
#include <asm/mmu.h>
#include <asm/security.h>
#include <asm/sysreg.h>

static struct arm64_cpu_caps g_fake_caps;

const struct arm64_cpu_caps *arm64_get_cpu_caps(void)
{
    return &g_fake_caps;
}

int main(void)
{
    struct arm64_cpu_caps caps;
    struct arm64_pac_keys k1;
    struct arm64_pac_keys k2;
    struct arm64_sec_status st;
    uint64_t sctlr;
    uint64_t pte_bti;
    uint64_t pte_data_noexec;
    uint64_t pte_mte;
    uint64_t pte_dev_mte;

    /* 1. 验证无扩展旧核（Cortex-A53）保持原有 SCTLR_EL1 控制位完全不变 */
    memset(&caps, 0, sizeof(caps));
    sctlr = arm64_security_compute_sctlr(0x1005ULL, &caps);
    assert(sctlr == 0x1005ULL);

    /* 2. 验证 PAC + BTI 组合配置 */
    memset(&caps, 0, sizeof(caps));
    caps.has_pac = true;
    caps.has_bti = true;
    sctlr = arm64_security_compute_sctlr(0ULL, &caps);
    assert((sctlr & (SCTLR_EL1_ENIA | SCTLR_EL1_ENIB | SCTLR_EL1_ENDA | SCTLR_EL1_ENDB)) ==
           (SCTLR_EL1_ENIA | SCTLR_EL1_ENIB | SCTLR_EL1_ENDA | SCTLR_EL1_ENDB));
    assert((sctlr & (SCTLR_EL1_BT0 | SCTLR_EL1_BT)) == (SCTLR_EL1_BT0 | SCTLR_EL1_BT));
    assert((sctlr & (SCTLR_EL1_ATA0 | SCTLR_EL1_TCF0_SYNC)) == 0ULL);

    /* 3. 验证 MTE1（无同步故障）vs MTE2（完整同步检查 + 不误伤无标签 EL1 TCF_SYNC） */
    caps.mte_version = 1U;
    caps.has_mte = true;
    sctlr = arm64_security_compute_sctlr(0ULL, &caps);
    assert((sctlr & (SCTLR_EL1_ATA0 | SCTLR_EL1_ATA | SCTLR_EL1_TCF0_SYNC)) == 0ULL);

    caps.mte_version = 2U;
    sctlr = arm64_security_compute_sctlr(0ULL, &caps);
    assert((sctlr & (SCTLR_EL1_ATA0 | SCTLR_EL1_ATA | SCTLR_EL1_TCF0_SYNC)) ==
           (SCTLR_EL1_ATA0 | SCTLR_EL1_ATA | SCTLR_EL1_TCF0_SYNC));
    assert((sctlr & SCTLR_EL1_TCF_SYNC) == 0ULL);

    /* 4. 验证 arm64_security_init_cpu 与 arm64_security_get_status 状态汇总 */
    g_fake_caps = caps;
    arm64_security_init_cpu();
    arm64_security_get_status(&st);
    assert(st.pac_enabled && st.bti_enabled && st.mte_enabled);
    assert(st.mte_version == 2U);
    assert(!st.cca_realm_present);
    assert((st.tcr_bits & TCR_EL1_TBI0) != 0ULL);

    /* 5. 验证 Per-Task 128-bit PAC 密钥高熵且互不相同、非全零 */
    arm64_pac_generate_keys(&k1, 1001ULL);
    arm64_pac_generate_keys(&k2, 1002ULL);
    assert(k1.initialized && k2.initialized);
    assert(k1.apia_lo != 0ULL && k1.apia_hi != 0ULL);
    assert(k1.apia_lo != k2.apia_lo || k1.apia_hi != k2.apia_hi);

    /* 6. 验证页表 PTE ARM64_PTE_GP (BTI) 与 MT_NORMAL_TAGGED (MTE) 编码 */
    pte_bti = arm64_pte_make_leaf(0x41000000UL,
                                  HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC | HAL_VM_PROT_BTI,
                                  false, false, true);
    assert((pte_bti & ARM64_PTE_GP) != 0ULL);
    assert((pte_bti & ARM64_PTE_UXN) == 0ULL);

    pte_data_noexec = arm64_pte_make_leaf(0x41001000UL,
                                          HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_BTI,
                                          false, false, true);
    assert((pte_data_noexec & ARM64_PTE_GP) == 0ULL);
    assert((pte_data_noexec & ARM64_PTE_UXN) != 0ULL);

    pte_mte = arm64_pte_make_leaf(0x41002000UL,
                                  HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_MTE,
                                  false, false, true);
    assert(((pte_mte >> 2) & 7ULL) == MT_NORMAL_TAGGED);
    assert(((ARM64_MAIR_VALUE >> (MT_NORMAL_TAGGED * 8)) & 0xFFULL) == 0xF0ULL);

    pte_dev_mte = arm64_pte_make_leaf(0x09000000UL,
                                      HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV | HAL_VM_PROT_MTE,
                                      false, false, false);
    assert(((pte_dev_mte >> 2) & 7ULL) == MT_DEVICE_nGnRnE);

    printf("PASS: test_arm64_security (PAC/BTI/MTE2 SCTLR & TCR, per-task keys, GP/Tagged PTEs)\n");
    return 0;
}
