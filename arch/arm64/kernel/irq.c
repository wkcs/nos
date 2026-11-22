/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 中断控制器驱动（GICv3 系统寄存器主路径 + GICv2 MMIO 自动兼容）与动态 IRQ 分发
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <asm/gic.h>
#include <asm/irq.h>

#ifndef __HOST_TEST__
#include <kernel/kernel.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/spinlock.h>
#include <kernel/printk.h>
#include <nos/sched.h>
#include <nos/hal_arch.h>
#include <asm/mmu.h>
#include <asm/sysreg.h>
#include <asm/cpufeature.h>
#include <asm/percpu.h>

extern void irq_entry(void);
extern void irq_exit(void);
#endif

struct arm64_irq_desc {
    nos_irq_handler_t handler;
    void             *ctx;
};

static struct arm64_irq_desc g_irq_table[GIC_MAX_IRQS];

void arm64_irq_table_reset(void)
{
    memset(g_irq_table, 0, sizeof(g_irq_table));
}

int nos_irq_register(uint32_t intid, nos_irq_handler_t handler, void *ctx)
{
    if (intid >= GIC_MAX_IRQS || handler == NULL) {
        return -EINVAL;
    }
    if (g_irq_table[intid].handler != NULL) {
        return -EBUSY;
    }
    g_irq_table[intid].handler = handler;
    g_irq_table[intid].ctx = ctx;
    return 0;
}

int nos_irq_unregister(uint32_t intid)
{
    if (intid >= GIC_MAX_IRQS) {
        return -EINVAL;
    }
    if (g_irq_table[intid].handler == NULL) {
        return -ENOENT;
    }
    g_irq_table[intid].handler = NULL;
    g_irq_table[intid].ctx = NULL;
    return 0;
}

int arm64_irq_dispatch_intid(uint32_t intid)
{
    if (intid >= GIC_MAX_IRQS || g_irq_table[intid].handler == NULL) {
        return -ENOENT;
    }
    g_irq_table[intid].handler(intid, g_irq_table[intid].ctx);
    return 0;
}

#ifndef __HOST_TEST__
extern void arm64_timer_enable_irq(void);
extern void arm64_do_context_switch(addr_t from_sp_ptr, addr_t to_sp_ptr);

static volatile uint8_t *g_gicd_base;
static volatile uint8_t *g_gicc_base;
static volatile uint8_t *g_gicr_base;
static bool g_use_gicv3;
static bool g_gic_ready;
static uint32_t g_max_irqs = 256U;

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline void mmio_write64(volatile uint8_t *base, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(base + off) = val;
}

static void gicd_wait_rwp(void)
{
    if (g_gicd_base == NULL) {
        return;
    }
    while ((mmio_read32(g_gicd_base, GICD_CTLR) & GICD_CTLR_RWP) != 0U) {
    }
}

static volatile uint8_t *gicr_cpu_base(uint32_t cpu_id)
{
    if (g_gicr_base == NULL) {
        return NULL;
    }
    return g_gicr_base + ((uintptr_t)cpu_id * GICR_STRIDE);
}

bool arm64_gic_is_v3(void)
{
    return g_use_gicv3;
}

void arm64_gic_cpu_init(uint32_t cpu_id)
{
    if (!g_gic_ready) {
        return;
    }

    if (g_use_gicv3) {
        volatile uint8_t *rbase = gicr_cpu_base(cpu_id);
        uint32_t waker;

        if (rbase != NULL) {
            /* 唤醒当前核 Redistributor，清除 ProcessorSleep 并等待 ChildrenAsleep=0 */
            waker = mmio_read32(rbase, GICR_WAKER);
            waker &= ~GICR_WAKER_PROC_SLEEP;
            mmio_write32(rbase, GICR_WAKER, waker);
            while ((mmio_read32(rbase, GICR_WAKER) & GICR_WAKER_CHILD_ASLEEP) != 0U) {
            }

            /* SGI/PPI 归入 Group 1 Non-Secure */
            mmio_write32(rbase, GICR_IGROUPR0, 0xFFFFFFFFU);
            mmio_write32(rbase, GICR_IGRPMODR0, 0x00000000U);
            mmio_write32(rbase, GICR_ICENABLER0, 0xFFFFFFFFU);
            for (uint32_t i = 0U; i < 8U; i++) {
                mmio_write32(rbase, GICR_IPRIORITYR(i), 0xA0A0A0A0U);
            }
        }

#if defined(__aarch64__)
        write_sysreg_s(read_sysreg_s(SYS_ICC_SRE_EL1) | 1ULL, SYS_ICC_SRE_EL1);
        __asm__ volatile("isb" ::: "memory");
        write_sysreg_s(0xFFULL, SYS_ICC_PMR_EL1);
        write_sysreg_s(0ULL, SYS_ICC_BPR1_EL1);
        write_sysreg_s(1ULL, SYS_ICC_IGRPEN1_EL1);
        __asm__ volatile("isb" ::: "memory");
#endif
    } else if (g_gicc_base != NULL) {
        /* GICv2 CPU Interface 与 SGI/PPI 初始化 */
        mmio_write32(g_gicd_base, GICD_ICENABLER(0), 0xFFFFFFFFU);
        for (uint32_t i = 0U; i < 8U; i++) {
            mmio_write32(g_gicd_base, GICD_IPRIORITYR(i), 0xA0A0A0A0U);
        }
        mmio_write32(g_gicc_base, GICC_PMR, 0xFFU);
        mmio_write32(g_gicc_base, GICC_BPR, 0U);
        mmio_write32(g_gicc_base, GICC_CTLR, 1U);
    }
}

int arm64_gic_init(const struct arm64_gic_config *cfg)
{
    uint32_t typer;
    uint32_t spi_lines;

    if (cfg == NULL || cfg->gicd_phys == 0U) {
        return -EINVAL;
    }

    g_gicd_base = (volatile uint8_t *)arm64_ioremap(cfg->gicd_phys,
                                                    cfg->gicd_size ? cfg->gicd_size : 0x10000UL);
    g_use_gicv3 = cfg->prefer_gicv3 &&
                  (cfg->gicr_phys != 0U) &&
                  arm64_get_cpu_caps()->has_gicv3;

    if (g_use_gicv3) {
        g_gicr_base = (volatile uint8_t *)arm64_ioremap(cfg->gicr_phys,
                                                        cfg->gicr_size ? cfg->gicr_size : 0x200000UL);
        /* 探测 GICD_PIDR2 确认真实 GIC 架构版本；若主版本非 3 则回退 GICv2 */
        uint32_t pidr2 = mmio_read32(g_gicd_base, 0xFFE8U);
        uint32_t arch_rev = (pidr2 >> 4) & 0xFU;
        if (arch_rev < 3U && cfg->gicc_phys != 0U) {
            g_use_gicv3 = false;
        }
    }

    if (!g_use_gicv3) {
        if (cfg->gicc_phys == 0U) {
            return -ENODEV;
        }
        g_gicc_base = (volatile uint8_t *)arm64_ioremap(cfg->gicc_phys,
                                                        cfg->gicc_size ? cfg->gicc_size : 0x10000UL);
    }

    typer = mmio_read32(g_gicd_base, GICD_TYPER);
    spi_lines = 32U * ((typer & 0x1FU) + 1U);
    g_max_irqs = (spi_lines < GIC_MAX_IRQS) ? spi_lines : GIC_MAX_IRQS;

    mmio_write32(g_gicd_base, GICD_CTLR, 0U);
    if (g_use_gicv3) {
        gicd_wait_rwp();
    }

    /* 初始化所有 SPI 中断：Group 1 NS、全禁能、默认优先级 0xA0、路由至 CPU0 */
    for (uint32_t i = 1U; i < (g_max_irqs / 32U); i++) {
        if (g_use_gicv3) {
            mmio_write32(g_gicd_base, GICD_IGROUPR(i), 0xFFFFFFFFU);
        }
        mmio_write32(g_gicd_base, GICD_ICENABLER(i), 0xFFFFFFFFU);
    }
    for (uint32_t i = 8U; i < (g_max_irqs / 4U); i++) {
        mmio_write32(g_gicd_base, GICD_IPRIORITYR(i), 0xA0A0A0A0U);
        if (!g_use_gicv3) {
            mmio_write32(g_gicd_base, GICD_ITARGETSR(i), 0x01010101U);
        }
    }

    if (g_use_gicv3) {
        mmio_write32(g_gicd_base, GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
        gicd_wait_rwp();
        for (uint32_t intid = GIC_SPI_BASE; intid < g_max_irqs; intid++) {
            mmio_write64(g_gicd_base, GICD_IROUTER(intid), 0ULL);
        }
    } else {
        mmio_write32(g_gicd_base, GICD_CTLR, 1U);
    }

    g_gic_ready = true;
    arm64_gic_cpu_init(0U);

    /* 若此前 cpu_init 已配置好通用定时器，立即放行其 PPI 30 中断 */
    arm64_timer_enable_irq();

    pr_info("ARM64 GIC initialized (%s, max_irqs=%u)\n",
            g_use_gicv3 ? "GICv3 SysReg" : "GICv2 MMIO", g_max_irqs);
    return 0;
}

void arm64_gic_enable_irq(uint32_t intid)
{
    if (!g_gic_ready || intid >= GIC_MAX_IRQS) {
        return;
    }
    if (g_use_gicv3 && intid < GIC_SPI_BASE) {
        volatile uint8_t *rbase = gicr_cpu_base(arm64_this_percpu()->cpu_id);
        if (rbase != NULL) {
            mmio_write32(rbase, GICR_ISENABLER0, 1U << intid);
        }
        return;
    }
    mmio_write32(g_gicd_base, GICD_ISENABLER(intid / 32U), 1U << (intid % 32U));
}

void arm64_gic_disable_irq(uint32_t intid)
{
    if (!g_gic_ready || intid >= GIC_MAX_IRQS) {
        return;
    }
    if (g_use_gicv3 && intid < GIC_SPI_BASE) {
        volatile uint8_t *rbase = gicr_cpu_base(arm64_this_percpu()->cpu_id);
        if (rbase != NULL) {
            mmio_write32(rbase, GICR_ICENABLER0, 1U << intid);
        }
        return;
    }
    mmio_write32(g_gicd_base, GICD_ICENABLER(intid / 32U), 1U << (intid % 32U));
}

void arm64_gic_set_priority(uint32_t intid, uint8_t prio)
{
    if (!g_gic_ready || intid >= GIC_MAX_IRQS) {
        return;
    }
    volatile uint8_t *reg_base = (g_use_gicv3 && intid < GIC_SPI_BASE)
        ? (gicr_cpu_base(arm64_this_percpu()->cpu_id) + GICR_SGI_OFFSET + 0x0400U)
        : (g_gicd_base + 0x0400U);
    *(reg_base + intid) = prio;
}

uint32_t arm64_gic_ack(void)
{
    if (!g_gic_ready) {
        return GIC_SPURIOUS_INTID;
    }
#if defined(__aarch64__)
    if (g_use_gicv3) {
        return (uint32_t)(read_sysreg_s(SYS_ICC_IAR1_EL1) & 0xFFFFFFULL);
    }
#endif
    return mmio_read32(g_gicc_base, GICC_IAR) & 0x3FFU;
}

void arm64_gic_eoi(uint32_t intid)
{
    if (!g_gic_ready || intid >= GIC_MAX_IRQS) {
        return;
    }
#if defined(__aarch64__)
    if (g_use_gicv3) {
        write_sysreg_s((uint64_t)intid, SYS_ICC_EOIR1_EL1);
        __asm__ volatile("isb" ::: "memory");
        return;
    }
#endif
    mmio_write32(g_gicc_base, GICC_EOIR, intid);
}

void arm64_gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id)
{
    if (!g_gic_ready || sgi_id >= GIC_SGI_NR || target_cpu >= 16U) {
        return;
    }
#if defined(__aarch64__)
    if (g_use_gicv3) {
        uint64_t val = ((uint64_t)sgi_id << 24) | (1ULL << target_cpu);
        write_sysreg_s(val, SYS_ICC_SGI1R_EL1);
        __asm__ volatile("isb" ::: "memory");
        return;
    }
#endif
    mmio_write32(g_gicd_base, 0x0F00U, ((1U << target_cpu) << 16) | sgi_id);
}

void arm64_irq_dispatch(void)
{
    uint32_t intid = arm64_gic_ack();
    if (intid >= GIC_MAX_IRQS) {
        return;
    }
    irq_entry();
    (void)arm64_irq_dispatch_intid(intid);
    irq_exit();
    arm64_gic_eoi(intid);
}

/* 在异常退出前（当前任务内核栈上且 interrupt_nest==0）安全执行延迟抢占切换 */
void arm64_irq_exit_resched(void)
{
    struct arm64_percpu *pcpu = arm64_this_percpu();
    struct sched_rq *rq = this_rq();

    if (pcpu != NULL && pcpu->deferred_switch) {
        addr_t from = pcpu->deferred_from;
        addr_t to = pcpu->deferred_to;
        pcpu->deferred_switch = false;
        pcpu->deferred_from = 0U;
        pcpu->deferred_to = 0U;

        spin_lock_irq(&rq->lock);
        if (rq->curr != NULL) {
            hal_vm_switch(&rq->curr->vm_space);
        }
        arm64_do_context_switch(from, to);
        spin_unlock_irq(&rq->lock);
        return;
    }

    if (kernel_running && scheduler_lock_nest == 0U && rq != NULL && rq->need_resched) {
        switch_task();
    }
}

/* ---------------- HAL 与传统 ASM 中断接口实现 ---------------- */

addr_t asm_disable_irq_save(void)
{
    addr_t flags = 0U;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(flags) :: "memory");
#endif
    return flags;
}

void asm_enable_irq_save(addr_t level)
{
#if defined(__aarch64__)
    __asm__ volatile("msr daif, %0" :: "r"(level) : "memory");
#else
    (void)level;
#endif
}

void arch_irq_init(void)
{
}

void arch_irq_enable(unsigned int irq)
{
    arm64_gic_enable_irq((uint32_t)irq);
}

void arch_irq_disable(unsigned int irq)
{
    arm64_gic_disable_irq((uint32_t)irq);
}

void arch_irq_set_priority(unsigned int irq, unsigned int priority)
{
    arm64_gic_set_priority((uint32_t)irq, (uint8_t)priority);
}

unsigned long arch_irq_save(void)
{
    return (unsigned long)asm_disable_irq_save();
}

void arch_irq_restore(unsigned long flags)
{
    asm_enable_irq_save((addr_t)flags);
}

void arch_irq_enable_global(void)
{
#if defined(__aarch64__)
    __asm__ volatile("msr daifclr, #2" ::: "memory");
#endif
}

void arch_irq_disable_global(void)
{
#if defined(__aarch64__)
    __asm__ volatile("msr daifset, #2" ::: "memory");
#endif
}
#endif /* !__HOST_TEST__ */
