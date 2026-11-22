/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 系统寄存器定义与安全读写封装
 * 注意：GICv3 ICC_* 寄存器使用通用 S3_* 系统编码，保证 -march=armv8-a 工具链零警告汇编。
 */

#ifndef __ASM_ARM64_SYSREG_H__
#define __ASM_ARM64_SYSREG_H__

/* SCTLR_EL1 控制位 */
#define SCTLR_EL1_M             (1ULL << 0)     /* 使能 EL1/EL0 MMU */
#define SCTLR_EL1_A             (1ULL << 1)     /* 对齐检查 */
#define SCTLR_EL1_C             (1ULL << 2)     /* 使能数据缓存 */
#define SCTLR_EL1_SA            (1ULL << 3)     /* EL1 SP 对齐检查 */
#define SCTLR_EL1_SA0           (1ULL << 4)     /* EL0 SP 对齐检查 */
#define SCTLR_EL1_I             (1ULL << 12)    /* 使能指令缓存 */
#define SCTLR_EL1_WXN           (1ULL << 19)    /* 硬件强制可写页不可执行 */
#define SCTLR_EL1_SPAN          (1ULL << 23)    /* 禁止异常进入时自动清除 PAN */
#define SCTLR_EL1_UCI           (1ULL << 26)    /* 允许 EL0 缓存清理指令 */

/* HCR_EL2 控制位 */
#define HCR_EL2_RW              (1ULL << 31)    /* EL1 运行于 AArch64 模式 */

/* CNTHCTL_EL2 控制位 */
#define CNTHCTL_EL2_EL1PCTEN    (1ULL << 0)     /* 允许 EL1 读取物理计数器 */
#define CNTHCTL_EL2_EL1PCEN     (1ULL << 1)     /* 允许 EL1 访问物理定时器 */

/* CPACR_EL1 控制位 */
#define CPACR_EL1_ZEN_SHIFT     16U             /* SVE 访问控制 */
#define CPACR_EL1_FPEN_SHIFT    20U             /* FP/ASIMD 访问控制 */
#define CPACR_EL1_SMEN_SHIFT    24U             /* SME 访问控制 */
#define CPACR_EL1_FPEN_MASK     (3ULL << CPACR_EL1_FPEN_SHIFT)
#define CPACR_EL1_FPEN_TRAP     (0ULL << CPACR_EL1_FPEN_SHIFT)
#define CPACR_EL1_FPEN_EL1      (1ULL << CPACR_EL1_FPEN_SHIFT)
#define CPACR_EL1_FPEN_FULL     (3ULL << CPACR_EL1_FPEN_SHIFT)

/* SPSR_ELx 状态位 */
#define PSR_MODE_EL0t           0x00000000ULL
#define PSR_MODE_EL1t           0x00000004ULL
#define PSR_MODE_EL1h           0x00000005ULL
#define PSR_F_BIT               (1ULL << 6)
#define PSR_I_BIT               (1ULL << 7)
#define PSR_A_BIT               (1ULL << 8)
#define PSR_D_BIT               (1ULL << 9)
#define PSR_DAIF_MASK           (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)

/* ESR_EL1 异常分类码 (EC = bits[31:26]) */
#define ESR_ELx_EC_SHIFT        26U
#define ESR_ELx_EC_MASK         0x3FU
#define ESR_ELx_EC(esr)         (((uint32_t)(esr) >> ESR_ELx_EC_SHIFT) & ESR_ELx_EC_MASK)
#define ESR_ELx_ISS(esr)        ((uint32_t)(esr) & 0x01FFFFFFU)

#define ESR_ELx_EC_UNKNOWN      0x00U
#define ESR_ELx_EC_FP_ASIMD     0x07U
#define ESR_ELx_EC_PAC          0x09U
#define ESR_ELx_EC_BTI          0x0DU
#define ESR_ELx_EC_SVC64        0x15U
#define ESR_ELx_EC_SVE          0x19U
#define ESR_ELx_EC_SME          0x1DU
#define ESR_ELx_EC_IABT_LOW     0x20U
#define ESR_ELx_EC_IABT_CUR     0x21U
#define ESR_ELx_EC_PC_ALIGN     0x22U
#define ESR_ELx_EC_DABT_LOW     0x24U
#define ESR_ELx_EC_DABT_CUR     0x25U
#define ESR_ELx_EC_SP_ALIGN     0x26U
#define ESR_ELx_EC_BRK64        0x3CU

/* GICv3 系统寄存器低层编码（兼容基础 armv8-a 汇编器） */
#define SYS_ICC_PMR_EL1         S3_0_C4_C6_0
#define SYS_ICC_IAR1_EL1        S3_0_C12_C12_0
#define SYS_ICC_EOIR1_EL1       S3_0_C12_C12_1
#define SYS_ICC_HPPIR1_EL1      S3_0_C12_C12_2
#define SYS_ICC_BPR1_EL1        S3_0_C12_C12_3
#define SYS_ICC_DIR_EL1         S3_0_C12_C11_1
#define SYS_ICC_SGI1R_EL1       S3_0_C12_C11_5
#define SYS_ICC_SRE_EL1         S3_0_C12_C12_5
#define SYS_ICC_IGRPEN1_EL1     S3_0_C12_C12_7
#define SYS_ICC_SRE_EL2         S3_4_C12_C9_5

#ifndef __ASSEMBLY__
#include <stdint.h>

#define __SYSREG_STR(x) #x
#define _SYSREG_STR(x)  __SYSREG_STR(x)

#define read_sysreg(reg) ({                                 \
    uint64_t __val;                                         \
    __asm__ volatile("mrs %0, " #reg : "=r"(__val));        \
    __val;                                                  \
})

#define write_sysreg(val, reg) do {                         \
    uint64_t __val = (uint64_t)(val);                       \
    __asm__ volatile("msr " #reg ", %0" :: "r"(__val) : "memory"); \
} while (0)

#define read_sysreg_s(enc) ({                               \
    uint64_t __val;                                         \
    __asm__ volatile("mrs %0, " _SYSREG_STR(enc) : "=r"(__val)); \
    __val;                                                  \
})

#define write_sysreg_s(val, enc) do {                       \
    uint64_t __val = (uint64_t)(val);                       \
    __asm__ volatile("msr " _SYSREG_STR(enc) ", %0" :: "r"(__val) : "memory"); \
} while (0)

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_ARM64_SYSREG_H__ */
