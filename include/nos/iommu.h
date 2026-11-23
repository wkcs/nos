/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 统一可移植 IOMMU 与 SVA/PASID 硬件隔离契约接口
 * 为 AI 加速器（SMMUv3 / RKNPU IOMMU 等）与微内核 Cell 地址空间提供统一 DMA 翻译约束。
 */

#ifndef __NOS_IOMMU_H__
#define __NOS_IOMMU_H__

#include <stdbool.h>
#include <stdint.h>

struct nos_vm_space;

struct nos_iommu_info {
    bool     hw_present;        /* true=硬件 IOMMU（如 ARM SMMUv3）已激活；false=软件影子隔离模式 */
    uint8_t  arch_rev;          /* 架构修订版本号（如 SMMUv3.0~3.3） */
    uint8_t  sid_bits;          /* 硬件支持的 StreamID 位宽 */
    uint8_t  ssid_bits;         /* 硬件支持的 SubstreamID (PASID) 位宽 */
    uint32_t max_streams;       /* 本驱动实例管理的 Stream 上限 */
    uint32_t max_pasids;        /* 每 Stream 支持的 PASID 上限 */
    uint32_t active_bindings;   /* 当前已绑定的 (StreamID, PASID) 数量 */
    uint64_t cmdq_submitted;    /* 已向硬件命令队列提交并同步的命令总数 */
    uint64_t irq_events;        /* 捕获到的硬件 EVTQ 异常事件计数 */
};

/**
 * 将用户/Cell 虚拟地址空间绑定至指定设备 StreamID 与 SubstreamID (PASID)
 * 实现硬件 SVA（Shared Virtual Addressing）统一地址空间访问，并强制禁绝 TTBR1 内核 DMA。
 */
int nos_iommu_bind_space(uint32_t stream_id, uint32_t pasid, struct nos_vm_space *space);

/**
 * 解绑指定 (StreamID, PASID) 映射并刷清硬件 TLB / 上下文缓存
 */
int nos_iommu_unbind_space(uint32_t stream_id, uint32_t pasid);

/**
 * 根据绑定至 (StreamID, PASID) 的 SVA 页表翻译设备虚拟地址 IOVA -> 物理地址及权限位
 */
int nos_iommu_translate(uint32_t stream_id,
                        uint32_t pasid,
                        uintptr_t iova,
                        uintptr_t *out_pa,
                        uint32_t *out_prot);

/**
 * 按 ASID 或 VA 主动广播失效 IOMMU 缓存条目
 */
void nos_iommu_tlbi_asid(uint16_t asid);
void nos_iommu_tlbi_va(uint16_t asid, uintptr_t va);

/**
 * 读取当前 IOMMU 状态摘要
 */
int nos_iommu_get_status(struct nos_iommu_info *out);

#endif /* __NOS_IOMMU_H__ */
