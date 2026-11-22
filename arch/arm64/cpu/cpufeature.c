/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 运行时 CPU 特性探测框架（基于 ID_AA64* 寄存器探测 ARMv8.0 ~ ARMv9 扩展）
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

static struct arm64_cpu_caps g_cpu_caps;

static uint8_t decode_pa_bits(uint64_t parange)
{
    static const uint8_t bits_map[7] = { 32U, 36U, 40U, 42U, 44U, 48U, 52U };
    return (parange < 7ULL) ? bits_map[parange] : 40U;
}

void arm64_cpufeature_init(uint8_t boot_el)
{
    uint64_t pfr0 = 0ULL;
    uint64_t pfr1 = 0ULL;
    uint64_t isar0 = 0ULL;
    uint64_t isar1 = 0ULL;
    uint64_t mmfr0 = 0ULL;
    uint64_t mmfr1 = 0ULL;
    uint64_t ctr = 0ULL;

#if defined(__aarch64__)
    pfr0  = read_sysreg(id_aa64pfr0_el1);
    pfr1  = read_sysreg_s(S3_0_C0_C4_1);    /* ID_AA64PFR1_EL1 */
    isar0 = read_sysreg(id_aa64isar0_el1);
    isar1 = read_sysreg(id_aa64isar1_el1);
    mmfr0 = read_sysreg(id_aa64mmfr0_el1);
    mmfr1 = read_sysreg(id_aa64mmfr1_el1);
    ctr   = read_sysreg(ctr_el0);
#endif

    /* ID_AA64PFR0_EL1 位域解码 */
    uint8_t fp_f = (uint8_t)((pfr0 >> 16) & 0xFU);
    uint8_t simd_f = (uint8_t)((pfr0 >> 20) & 0xFU);
    g_cpu_caps.has_fp    = (fp_f != 0xFU);
    g_cpu_caps.has_fp16  = (fp_f == 1U);
    g_cpu_caps.has_asimd = (simd_f != 0xFU);
    g_cpu_caps.has_gicv3 = (((pfr0 >> 24) & 0xFU) != 0ULL);
    g_cpu_caps.has_sve   = (((pfr0 >> 32) & 0xFU) != 0ULL);

    /* ID_AA64PFR1_EL1 (BTI / MTE / SME) */
    g_cpu_caps.has_bti   = ((pfr1 & 0xFU) >= 1ULL);
    g_cpu_caps.has_mte   = (((pfr1 >> 8) & 0xFU) >= 1ULL);
    g_cpu_caps.has_sme   = (((pfr1 >> 24) & 0xFU) >= 1ULL);

    /* ID_AA64ISAR0_EL1 (LSE / DotProd) */
    g_cpu_caps.has_lse     = (((isar0 >> 20) & 0xFU) >= 2ULL);
    g_cpu_caps.has_dotprod = (((isar0 >> 44) & 0xFU) >= 1ULL);

    /* ID_AA64ISAR1_EL1 (PAC / BF16 / I8MM) */
    g_cpu_caps.has_pac  = ((((isar1 >> 4) & 0xFU) != 0ULL) ||
                           (((isar1 >> 8) & 0xFU) != 0ULL) ||
                           (((isar1 >> 24) & 0xFU) != 0ULL) ||
                           (((isar1 >> 28) & 0xFU) != 0ULL));
    g_cpu_caps.has_bf16 = (((isar1 >> 44) & 0xFU) >= 1ULL);
    g_cpu_caps.has_i8mm = (((isar1 >> 52) & 0xFU) >= 1ULL);

    /* ID_AA64MMFR0/1_EL1 */
    g_cpu_caps.pa_bits   = decode_pa_bits(mmfr0 & 0xFU);
    g_cpu_caps.has_asid16 = (((mmfr0 >> 4) & 0xFU) == 2ULL);
    g_cpu_caps.has_afdbm = ((mmfr1 & 0xFU) >= 1ULL);
    g_cpu_caps.has_pan   = (((mmfr1 >> 20) & 0xFU) >= 1ULL);

    g_cpu_caps.boot_el     = boot_el;
    g_cpu_caps.dcache_line = 4U << ((uint32_t)((ctr >> 16) & 0xFU));
    g_cpu_caps.icache_line = 4U << ((uint32_t)(ctr & 0xFU));
}

const struct arm64_cpu_caps *arm64_get_cpu_caps(void)
{
    return &g_cpu_caps;
}

void arm64_cpufeature_dump(void)
{
    const struct arm64_cpu_caps *c = &g_cpu_caps;
    pr_info("ARM64 CPU: boot=EL%u->EL1 PA=%u-bit ASID=%u-bit dline=%uB\n",
            (unsigned int)c->boot_el,
            (unsigned int)c->pa_bits,
            c->has_asid16 ? 16U : 8U,
            c->dcache_line);
    pr_info("ARM64 ISA: %s%s%s%s%s%s%s%s%s%s%s%s%s\n",
            c->has_fp ? "FP " : "",
            c->has_asimd ? "ASIMD " : "",
            c->has_lse ? "LSE " : "",
            c->has_pan ? "PAN " : "",
            c->has_afdbm ? "AFDBM " : "",
            c->has_gicv3 ? "GICv3 " : "GICv2 ",
            c->has_dotprod ? "DOTPROD " : "",
            c->has_fp16 ? "FP16 " : "",
            c->has_bf16 ? "BF16 " : "",
            c->has_i8mm ? "I8MM " : "",
            c->has_sve ? "SVE " : "",
            c->has_pac ? "PAC " : "",
            c->has_bti ? "BTI " : "");
}
