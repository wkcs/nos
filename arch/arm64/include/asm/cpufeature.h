/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 运行时 CPU 特性探测框架（兼容 ARMv8.0 ~ ARMv9-A 与 AI 扩展探测）
 */

#ifndef __ASM_ARM64_CPUFEATURE_H__
#define __ASM_ARM64_CPUFEATURE_H__

#include <stdint.h>
#include <stdbool.h>

struct arm64_cpu_caps {
    bool     has_fp;
    bool     has_asimd;
    bool     has_lse;       /* ARMv8.1 大规模系统扩展原子指令 */
    bool     has_pan;       /* 特权禁止访问用户内存 */
    bool     has_afdbm;     /* 硬件 Access/Dirty 位自动管理 */
    bool     has_asid16;    /* 16-bit ASID (65535 空间隔离) */
    bool     has_gicv3;     /* GICv3/v4 系统寄存器接口 */
    bool     has_dotprod;   /* ASIMD 点积加速 (AI 量化推理关键指令) */
    bool     has_fp16;      /* 半精度浮点 */
    bool     has_bf16;      /* BFloat16 AI 算力指令 */
    bool     has_i8mm;      /* Int8 矩阵乘加速 */
    bool     has_sve;       /* 可伸缩向量扩展 */
    bool     has_sve2;      /* ARMv9 SVE2 */
    bool     has_sme;       /* 可伸缩矩阵扩展 */
    bool     has_pac;       /* 指针认证防 ROP */
    bool     has_bti;       /* 分支目标防 JOP */
    bool     has_mte;       /* 内存标签防堆越界 */
    uint8_t  mte_version;   /* ID_AA64PFR1_EL1.MTE 版本 (0=无, 1=指令级, 2=完整同步检查) */
    uint8_t  pa_bits;       /* 物理地址位宽 (32..52) */
    uint8_t  boot_el;       /* 引导时入口异常级别 (1 或 2) */
    uint32_t dcache_line;   /* D-Cache 行大小 (字节) */
    uint32_t icache_line;   /* I-Cache 行大小 (字节) */
};

void arm64_cpufeature_init(uint8_t boot_el);
const struct arm64_cpu_caps *arm64_get_cpu_caps(void);
void arm64_cpufeature_dump(void);
void arm64_topo_classify_from_caps(const uint32_t *caps,
                                   uint32_t nr_cpus,
                                   uint32_t online_mask,
                                   uint32_t *out_big_mask,
                                   uint32_t *out_little_mask);
void arm64_topo_classify_masks(uint32_t online_mask,
                               uint32_t *out_big_mask,
                               uint32_t *out_little_mask);

#endif /* __ASM_ARM64_CPUFEATURE_H__ */
