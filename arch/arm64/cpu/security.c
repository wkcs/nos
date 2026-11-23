/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARMv8.3 PAC / ARMv8.5 BTI / MTE2 硬件安全硬化与 CCA/RMM 扩展探测
 */

#include <asm/security.h>
#include <asm/sysreg.h>
#include <kernel/kernel.h>
#include <string.h>

#ifndef __HOST_TEST__
#include <asm/percpu.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#else
#ifndef pr_info
#define pr_info(...) ((void)0)
#endif
#endif

static struct arm64_sec_status g_sec_status;

static uint64_t splitmix64_step(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void arm64_pac_generate_keys(struct arm64_pac_keys *out, uint64_t entropy)
{
    uint64_t st;

    if (out == NULL) {
        return;
    }

    st = (entropy != 0ULL) ? entropy : 0x5A3C96E1B472D80FULL;
    out->apia_lo = splitmix64_step(&st);
    out->apia_hi = splitmix64_step(&st);
    if (out->apia_lo == 0ULL) {
        out->apia_lo = 0xA55AA55A01234567ULL;
    }
    if (out->apia_hi == 0ULL) {
        out->apia_hi = 0xCAFEBABE89ABCDEFULL;
    }
    out->initialized = true;
}

uint64_t arm64_security_compute_sctlr(uint64_t base_sctlr, const struct arm64_cpu_caps *caps)
{
    uint64_t sctlr = base_sctlr;

    if (caps == NULL) {
        return sctlr;
    }

    if (caps->has_pac) {
        sctlr |= SCTLR_EL1_ENIA | SCTLR_EL1_ENIB | SCTLR_EL1_ENDA | SCTLR_EL1_ENDB;
    }
    if (caps->has_bti) {
        sctlr |= SCTLR_EL1_BT0 | SCTLR_EL1_BT;
    }
    if (caps->mte_version >= 2U) {
        /* 仅启用 EL0 同步标签检查与标签指令访问，保持无标签 EL1 内核指针零误报 */
        sctlr |= SCTLR_EL1_ATA0 | SCTLR_EL1_ATA | SCTLR_EL1_TCF0_SYNC;
    }
    return sctlr;
}

bool arm64_cca_probe(void)
{
#if defined(CONFIG_ARM64_CCA_PROBE) && defined(__aarch64__) && !defined(__HOST_TEST__)
    register uint64_t x0 __asm__("x0") = 0xC4000150ULL; /* SMC_RMI_VERSION */
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    /* RMME 1.0+ 返回版本编码非负且非 -1 (SMCCC_NOT_SUPPORTED) */
    return ((int64_t)x0 > 0);
#else
    return false;
#endif
}

void arm64_security_init_cpu(void)
{
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();
    uint64_t sctlr_val = 0ULL;
    uint64_t tcr_val = 0ULL;
    bool is_primary = true;

#if defined(__aarch64__) && !defined(__HOST_TEST__)
    struct arm64_percpu *pcpu = arm64_this_percpu();
    uint64_t cnt = read_sysreg(cntpct_el0);
    uint64_t seed = cnt ^ ((pcpu != NULL ? (uint64_t)pcpu->cpu_id : 0ULL) << 48) ^ 0x4E4F5353454334ULL;

    is_primary = (pcpu == NULL || pcpu->cpu_id == 0U);

    if (caps->has_pac) {
        struct arm64_pac_keys boot_key;
        arm64_pac_generate_keys(&boot_key, seed);
        write_sysreg_s(boot_key.apia_lo, SYS_APIAKEYLO_EL1);
        write_sysreg_s(boot_key.apia_hi, SYS_APIAKEYHI_EL1);
    }

    if (caps->mte_version >= 2U) {
        /* GCR_EL1: bit16 RRND=1 随机标签模式 + 排除 tag 0（保留给普通指针） */
        write_sysreg_s(0x10001ULL, SYS_GCR_EL1);
        write_sysreg_s((seed & 0xFFFEULL) | 0x1U, SYS_RGSR_EL1);
    }

    tcr_val = read_sysreg(tcr_el1);
    if (caps->has_pac || caps->mte_version >= 2U) {
        tcr_val |= TCR_EL1_TBI0;
        write_sysreg(tcr_val, tcr_el1);
        __asm__ volatile("isb" ::: "memory");
    }

    sctlr_val = arm64_security_compute_sctlr(read_sysreg(sctlr_el1), caps);
    write_sysreg(sctlr_val, sctlr_el1);
    __asm__ volatile("isb" ::: "memory");
#else
    sctlr_val = arm64_security_compute_sctlr(0ULL, caps);
    if (caps->has_pac || caps->mte_version >= 2U) {
        tcr_val |= TCR_EL1_TBI0;
    }
#endif

    if (is_primary) {
        g_sec_status.pac_enabled = caps->has_pac;
        g_sec_status.bti_enabled = caps->has_bti;
        g_sec_status.mte_enabled = (caps->mte_version >= 2U);
        g_sec_status.mte_version = caps->mte_version;
        g_sec_status.cca_realm_present = arm64_cca_probe();
        g_sec_status.sctlr_bits = sctlr_val;
        g_sec_status.tcr_bits = tcr_val;
    }
}

void arm64_security_on_context_switch(struct task_struct *next)
{
#if defined(__aarch64__) && !defined(__HOST_TEST__)
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();
    if (!caps->has_pac || next == NULL) {
        return;
    }
    if (!next->pac_keys.initialized) {
        uint64_t entropy = read_sysreg(cntpct_el0) ^
                           ((uint64_t)next->pid << 32) ^
                           (uint64_t)(uintptr_t)next;
        arm64_pac_generate_keys(&next->pac_keys, entropy);
    }
    write_sysreg_s(next->pac_keys.apia_lo, SYS_APIAKEYLO_EL1);
    write_sysreg_s(next->pac_keys.apia_hi, SYS_APIAKEYHI_EL1);
    __asm__ volatile("isb" ::: "memory");
#else
    (void)next;
#endif
}

void arm64_security_get_status(struct arm64_sec_status *out)
{
    if (out != NULL) {
        *out = g_sec_status;
    }
}

void arm64_security_dump(void)
{
    pr_info("ARM64 Security: PAC=%s BTI=%s MTE=%s(v%u) CCA=%s\n",
            g_sec_status.pac_enabled ? "on" : "off",
            g_sec_status.bti_enabled ? "on" : "off",
            g_sec_status.mte_enabled ? "on" : "off",
            (unsigned int)g_sec_status.mte_version,
            g_sec_status.cca_realm_present ? "realm" : "none");
}
