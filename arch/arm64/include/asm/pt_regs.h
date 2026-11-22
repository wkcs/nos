/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 异常现场保存帧定义（C 与汇编严格共享偏移常量，288 字节 16B 对齐）
 */

#ifndef __ASM_ARM64_PT_REGS_H__
#define __ASM_ARM64_PT_REGS_H__

#define PT_X0       0
#define PT_X1       8
#define PT_X2       16
#define PT_X3       24
#define PT_X4       32
#define PT_X5       40
#define PT_X6       48
#define PT_X7       56
#define PT_X8       64
#define PT_X18      144
#define PT_X28      224
#define PT_X29      232
#define PT_X30      240
#define PT_SP       248
#define PT_PC       256
#define PT_PSTATE   264
#define PT_ESR      272
#define PT_FAR      280
#define PT_SIZE     288

#ifndef __ASSEMBLY__
#include <stddef.h>
#include <stdint.h>

struct arm64_pt_regs {
    uint64_t regs[31];  /* x0 ~ x30 */
    uint64_t sp;        /* 被中断时的 sp_el0 或原 sp_el1 */
    uint64_t pc;        /* ELR_EL1 */
    uint64_t pstate;    /* SPSR_EL1 */
    uint64_t esr;       /* ESR_EL1 */
    uint64_t far;       /* FAR_EL1 */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct arm64_pt_regs) == PT_SIZE,
               "arm64_pt_regs 尺寸必须严格等于 PT_SIZE (288B)");
_Static_assert(offsetof(struct arm64_pt_regs, sp) == PT_SP, "PT_SP 偏移不匹配");
_Static_assert(offsetof(struct arm64_pt_regs, pc) == PT_PC, "PT_PC 偏移不匹配");
_Static_assert(offsetof(struct arm64_pt_regs, pstate) == PT_PSTATE, "PT_PSTATE 偏移不匹配");
_Static_assert(offsetof(struct arm64_pt_regs, esr) == PT_ESR, "PT_ESR 偏移不匹配");
_Static_assert(offsetof(struct arm64_pt_regs, far) == PT_FAR, "PT_FAR 偏移不匹配");
#endif

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_ARM64_PT_REGS_H__ */
