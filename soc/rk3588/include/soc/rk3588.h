/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Rockchip RK3588 SoC 外设与 3-Core RKNPU v2 硬件基址/寄存器契约定义
 */

#ifndef __SOC_RK3588_H__
#define __SOC_RK3588_H__

#include <stdint.h>

/* RK3588 3-Core NPU 寄存器基址（总计 6.0 TOPS INT8） */
#define RK3588_RKNPU_CORE0_BASE     0xFDAB0000UL
#define RK3588_RKNPU_CORE1_BASE     0xFDAC0000UL
#define RK3588_RKNPU_CORE2_BASE     0xFDAD0000UL
#define RK3588_RKNPU_IOMMU_OFFSET   0x0800U

/* RKNPU 子模块寄存器偏移 */
#define RKNPU_REG_PC_DATA_ADDR      0x0010U
#define RKNPU_REG_PC_DATA_AMOUNT    0x0014U
#define RKNPU_REG_PC_OP_EN          0x0008U
#define RKNPU_REG_PC_INTR_MASK      0x0020U
#define RKNPU_REG_PC_INTR_STATUS    0x0028U
#define RKNPU_REG_PC_INTR_CLR       0x0024U
#define RKNPU_REG_CNA_S_STATUS      0x1000U
#define RKNPU_REG_DPU_S_STATUS      0x4000U

/* RKNPU 硬件 Task Descriptor（严格 64 字节） */
struct rknpu_hw_task_desc {
    uint64_t regcfg_pa;
    uint32_t regcfg_amount;
    uint32_t flags;
    uint32_t int_mask;
    uint32_t int_clear;
    uint64_t input_iova;
    uint64_t weight_iova;
    uint64_t output_iova;
    uint32_t dim_m;
    uint32_t dim_n;
    uint32_t dim_k;
    uint32_t kernel_op;
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct rknpu_hw_task_desc) == 64, "rknpu_hw_task_desc 必须为 64 字节");
#endif

/**
 * 编码 RK3588 RKNPU IOMMU 二级 DTE 页表条目（4KB 页物理地址 + V/R/W 权限）
 */
static inline uint32_t rknpu_iommu_pte_make(uint64_t pa, int writable)
{
    uint32_t pte = (uint32_t)(pa & 0xFFFFF000ULL) | 0x1U; /* bit0 = Present */
    pte |= (1U << 2); /* Read */
    if (writable) {
        pte |= (1U << 1); /* Write */
    }
    return pte;
}

void rk3588_soc_init(void);

#endif /* __SOC_RK3588_H__ */
