/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 GICv3（主路径）与 GICv2（兼容回退）硬件抽象接口
 */

#ifndef __ASM_ARM64_GIC_H__
#define __ASM_ARM64_GIC_H__

#include <stdint.h>
#include <stdbool.h>

#define GIC_MAX_IRQS            1020U
#define GIC_SPURIOUS_INTID      1020U
#define GIC_SGI_NR              16U
#define GIC_PPI_BASE            16U
#define GIC_SPI_BASE            32U

/* GICD 公共寄存器偏移 */
#define GICD_CTLR               0x0000U
#define GICD_TYPER              0x0004U
#define GICD_IGROUPR(n)         (0x0080U + 4U * (n))
#define GICD_ISENABLER(n)       (0x0100U + 4U * (n))
#define GICD_ICENABLER(n)       (0x0180U + 4U * (n))
#define GICD_ISPENDR(n)         (0x0200U + 4U * (n))
#define GICD_ICPENDR(n)         (0x0280U + 4U * (n))
#define GICD_IPRIORITYR(n)      (0x0400U + 4U * (n))
#define GICD_ITARGETSR(n)       (0x0800U + 4U * (n))
#define GICD_ICFGR(n)           (0x0C00U + 4U * (n))
#define GICD_IROUTER(n)         (0x6000U + 8U * (n))

#define GICD_CTLR_ENABLE_G0     (1U << 0)
#define GICD_CTLR_ENABLE_G1NS   (1U << 1)
#define GICD_CTLR_ARE_NS        (1U << 4)
#define GICD_CTLR_RWP           (1U << 31)

/* GICR Redistributor 偏移 (每核 128KB 跨步：RD_base 64KB + SGI_base 64KB) */
#define GICR_STRIDE             0x20000U
#define GICR_SGI_OFFSET         0x10000U
#define GICR_WAKER              0x0014U
#define GICR_WAKER_PROC_SLEEP   (1U << 1)
#define GICR_WAKER_CHILD_ASLEEP (1U << 2)

#define GICR_IGROUPR0           (GICR_SGI_OFFSET + 0x0080U)
#define GICR_ISENABLER0         (GICR_SGI_OFFSET + 0x0100U)
#define GICR_ICENABLER0         (GICR_SGI_OFFSET + 0x0180U)
#define GICR_IPRIORITYR(n)      (GICR_SGI_OFFSET + 0x0400U + 4U * (n))
#define GICR_IGRPMODR0          (GICR_SGI_OFFSET + 0x0D00U)

/* GICv2 CPU Interface 偏移 */
#define GICC_CTLR               0x0000U
#define GICC_PMR                0x0004U
#define GICC_BPR                0x0008U
#define GICC_IAR                0x000CU
#define GICC_EOIR               0x0010U

struct arm64_gic_config {
    uintptr_t gicd_phys;
    size_t    gicd_size;
    uintptr_t gicc_phys;    /* GICv2 CPU 接口物理基址 */
    size_t    gicc_size;
    uintptr_t gicr_phys;    /* GICv3 Redistributor 物理基址 (无 GICv3 时传 0) */
    size_t    gicr_size;
    bool      prefer_gicv3;
};

int arm64_gic_init(const struct arm64_gic_config *cfg);
void arm64_gic_cpu_init(uint32_t cpu_id);
bool arm64_gic_is_v3(void);
uint32_t arm64_gic_ack(void);
void arm64_gic_eoi(uint32_t intid);
void arm64_gic_enable_irq(uint32_t intid);
void arm64_gic_disable_irq(uint32_t intid);
void arm64_gic_set_priority(uint32_t intid, uint8_t prio);
void arm64_gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id);

#endif /* __ASM_ARM64_GIC_H__ */
