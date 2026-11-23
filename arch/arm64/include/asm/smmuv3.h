/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM SMMUv3（ARM IHI 0070）MMIO 寄存器与硬件描述符定义
 */

#ifndef __ASM_ARM64_SMMUV3_H__
#define __ASM_ARM64_SMMUV3_H__

#include <nos/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* QEMU virt 默认 SMMUv3 MMIO 窗口基址与尺寸 */
#define ARM64_SMMUV3_DEFAULT_BASE       0x09050000UL
#define ARM64_SMMUV3_DEFAULT_SIZE       0x00020000UL

/* SMMUv3 硬件寄存器偏移 */
#define SMMU_IDR0                       0x0000U
#define SMMU_IDR1                       0x0004U
#define SMMU_AIDR                       0x001CU
#define SMMU_CR0                        0x0020U
#define SMMU_CR0ACK                     0x0024U
#define SMMU_CR1                        0x0028U
#define SMMU_CR2                        0x002CU
#define SMMU_IRQ_CTRL                   0x0050U
#define SMMU_IRQ_CTRLACK                0x0054U
#define SMMU_GERROR                     0x0060U
#define SMMU_GERRORN                    0x0064U
#define SMMU_STRTAB_BASE                0x0080U
#define SMMU_STRTAB_BASE_CFG            0x0088U
#define SMMU_CMDQ_BASE                  0x0090U
#define SMMU_CMDQ_PROD                  0x0098U
#define SMMU_CMDQ_CONS                  0x009CU
#define SMMU_EVTQ_BASE                  0x00A0U
#define SMMU_EVTQ_PROD                  0x00A8U
#define SMMU_EVTQ_CONS                  0x00ACU

/* IDR0 位域 */
#define SMMU_IDR0_S2P                   (1U << 0)
#define SMMU_IDR0_S1P                   (1U << 1)
#define SMMU_IDR0_TTF_SHIFT             2U
#define SMMU_IDR0_TTF_MASK              (3U << SMMU_IDR0_TTF_SHIFT)
#define SMMU_IDR0_TTF_AARCH64           (2U << SMMU_IDR0_TTF_SHIFT)
#define SMMU_IDR0_BTM                   (1U << 5)
#define SMMU_IDR0_ASID16                (1U << 12)

/* CR0 控制位 */
#define SMMU_CR0_SMMUEN                 (1U << 0)
#define SMMU_CR0_PRIQEN                 (1U << 1)
#define SMMU_CR0_EVENTQEN               (1U << 2)
#define SMMU_CR0_CMDQEN                 (1U << 3)

/* CR1 内存属性控制位（全 ISH + WB 缓存一致） */
#define SMMU_CR1_SH_ISH                 3U
#define SMMU_CR1_CACHE_WB               1U
#define SMMU_CR1_VAL ( \
    (SMMU_CR1_CACHE_WB << 0) | (SMMU_CR1_CACHE_WB << 2) | (SMMU_CR1_SH_ISH << 4) | \
    (SMMU_CR1_CACHE_WB << 6) | (SMMU_CR1_CACHE_WB << 8) | (SMMU_CR1_SH_ISH << 10)  \
)

/* CR2 控制位 */
#define SMMU_CR2_PTM                    (1U << 2)

/* IRQ_CTRL 中断使能控制位 */
#define SMMU_IRQ_CTRL_GERROR_IRQEN      (1U << 0)
#define SMMU_IRQ_CTRL_PRIQ_IRQEN        (1U << 1)
#define SMMU_IRQ_CTRL_EVENTQ_IRQEN      (1U << 2)

/* 队列与表尺寸规格（均严格对齐单个 4KB 页） */
#define SMMU_MAX_STREAMS                64U
#define SMMU_STRTAB_LOG2SIZE            6U          /* 64 * 64B = 4096B */
#define SMMU_MAX_PASIDS                 16U
#define SMMU_CD_TABLE_LOG2              4U          /* 16 * 64B = 1024B */
#define SMMU_CMDQ_LOG2SIZE              8U          /* 256 * 16B = 4096B */
#define SMMU_EVTQ_LOG2SIZE              7U          /* 128 * 32B = 4096B */

/* STE 配置字段（64 字节 = 8 * uint64_t） */
#define SMMU_STE0_V                     (1ULL << 0)
#define SMMU_STE0_CFG_ABORT             (0ULL << 1)
#define SMMU_STE0_CFG_BYPASS            (4ULL << 1)
#define SMMU_STE0_CFG_S1_TRANS          (5ULL << 1)
#define SMMU_STE0_S1FMT_LINEAR          (0ULL << 4)
#define SMMU_STE0_CTXPTR_MASK           0x000FFFFFFFFFFFC0ULL

#define SMMU_STE1_S1DSS_BYPASS          (1ULL << 6)
#define SMMU_STE1_S1DSS_SSID0           (2ULL << 6)
#define SMMU_STE1_S1CIR_WB              (1ULL << 16)
#define SMMU_STE1_S1COR_WB              (1ULL << 18)
#define SMMU_STE1_S1CSH_ISH             (3ULL << 20)
#define SMMU_STE1_S1STALLD              (1ULL << 27)
#define SMMU_STE1_SHCFG_INCOMING        (1ULL << 61)

/* Context Descriptor (CD) 字段（64 字节 = 8 * uint64_t） */
#define SMMU_CD0_TG0_4K                 (0ULL << 6)
#define SMMU_CD0_IR0_WBWA               (1ULL << 8)
#define SMMU_CD0_OR0_WBWA               (1ULL << 10)
#define SMMU_CD0_SH0_ISH                (3ULL << 12)
#define SMMU_CD0_EPD1                   (1ULL << 30) /* 强制禁用 TTBR1，堵死 DMA 越界读内核 */
#define SMMU_CD0_V                      (1ULL << 31)
#define SMMU_CD0_IPS_SHIFT              32U
#define SMMU_CD0_AFFD                   (1ULL << 35)
#define SMMU_CD0_WXN                    (1ULL << 36)
#define SMMU_CD0_AA64                   (1ULL << 41)
#define SMMU_CD0_R                      (1ULL << 45)
#define SMMU_CD0_A                      (1ULL << 46)
#define SMMU_CD0_ASET                   (1ULL << 47)
#define SMMU_CD0_ASID_SHIFT             48U

/* SMMUv3 CMDQ 命令操作码 */
#define SMMU_CMD_CFGI_STE               0x03U
#define SMMU_CMD_CFGI_ALL               0x04U
#define SMMU_CMD_CFGI_CD                0x05U
#define SMMU_CMD_TLBI_NH_ALL            0x10U
#define SMMU_CMD_TLBI_NH_ASID           0x11U
#define SMMU_CMD_TLBI_NH_VA             0x12U
#define SMMU_CMD_SYNC                   0x46U

struct smmuv3_ste {
    uint64_t dw[8];
};

struct smmuv3_cd {
    uint64_t dw[8];
};

struct smmuv3_cmd {
    uint64_t dw[2];
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct smmuv3_ste) == 64, "SMMUv3 STE 必须严格为 64 字节");
_Static_assert(sizeof(struct smmuv3_cd) == 64, "SMMUv3 CD 必须严格为 64 字节");
_Static_assert(sizeof(struct smmuv3_cmd) == 16, "SMMUv3 CMD 必须严格为 16 字节");
#endif

/**
 * 探测并初始化指定 MMIO 基址的 ARM SMMUv3；若硬件不存在则静默转入影子隔离模式
 */
int arm64_smmuv3_init(uintptr_t mmio_base, size_t mmio_size);

/**
 * 探测默认 QEMU virt SMMUv3 (0x09050000) 或设备树 arm,smmu-v3 节点
 */
int arm64_smmuv3_probe_default(void);

#ifdef __HOST_TEST__
struct arm64_pt_backend;

struct smmuv3_host_mmio_ops {
    uint32_t (*read32)(uintptr_t off, void *ctx);
    void     (*write32)(uintptr_t off, uint32_t val, void *ctx);
    uint64_t (*read64)(uintptr_t off, void *ctx);
    void     (*write64)(uintptr_t off, uint64_t val, void *ctx);
    void      *ctx;
};

void smmuv3_host_set_mmio_ops(const struct smmuv3_host_mmio_ops *ops);
void smmuv3_host_set_pt_backend(const struct arm64_pt_backend *be);
void smmuv3_host_inject_event(const struct nos_iommu_fault_event *evt);
void smmuv3_host_reset(void);
#endif

#endif /* __ASM_ARM64_SMMUV3_H__ */
