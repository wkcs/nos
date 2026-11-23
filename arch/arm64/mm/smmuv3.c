/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 SMMUv3 硬件 SVA/PASID 隔离驱动与 Shadow IOMMU 回退层
 */

#define pr_fmt(fmt) "[SMMUv3]:%s[%d]:"fmt, __func__, __LINE__

#include <asm/smmuv3.h>
#include <asm/mmu.h>
#include <nos/iommu.h>
#include <nos/hal_arch.h>
#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/spinlock.h>
#include <string.h>

#ifndef CONFIG_ARM64_VA_BITS
#define CONFIG_ARM64_VA_BITS 39
#endif

#ifdef __HOST_TEST__
#include <stdlib.h>
#ifndef pr_info
#define pr_info(...) ((void)0)
#endif
#else
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/fdt_scan.h>
#include <asm/gic.h>
#include <asm/irq.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <nos/devicetree.h>
#endif

struct smmu_pasid_binding {
    bool                 active;
    struct nos_vm_space *space;
    uintptr_t            root_pt_phys;
    uint16_t             asid;
};

struct smmu_stream_state {
    bool                      configured;
    struct smmuv3_cd         *cd_table;
    uintptr_t                 cd_table_phys;
    uint8_t                   s1cdmax;
    struct smmu_pasid_binding bindings[SMMU_MAX_PASIDS];
};

struct smmuv3_state {
    spinlock_t               lock;
    bool                     initialized;
    bool                     hw_present;
    volatile uint8_t        *mmio_vbase;
    uint32_t                 idr0;
    uint32_t                 idr1;
    uint32_t                 aidr;
    uint8_t                  sid_bits;
    uint8_t                  ssid_bits;
    uint32_t                 max_pasids;
    uint32_t                 cmdq_log2;
    uint32_t                 cmdq_prod;
    struct smmuv3_ste       *strtab;
    uintptr_t                strtab_phys;
    struct smmuv3_cmd       *cmdq;
    uintptr_t                cmdq_phys;
    uint8_t                 *evtq;
    uintptr_t                evtq_phys;
    uint64_t                 cmdq_submitted;
    uint64_t                 irq_events;
    uint32_t                 active_bindings;
    uint32_t                 evtq_irq;
    uint32_t                 gerror_irq;
    bool                     irq_registered;
    struct smmu_stream_state streams[SMMU_MAX_STREAMS];
};

static struct smmuv3_state g_smmu;

#define SMMU_MAX_FAULT_CBS 4U
static nos_iommu_fault_cb_t g_fault_cbs[SMMU_MAX_FAULT_CBS];

#ifdef __HOST_TEST__
static struct smmuv3_host_mmio_ops g_host_mmio;
static const struct arm64_pt_backend *g_host_pt_be;
static struct nos_iommu_fault_event g_host_inj_events[16];
static uint32_t g_host_inj_head;
static uint32_t g_host_inj_tail;
static uint32_t g_host_inj_cnt;
#endif

static void *smmu_alloc_page(uintptr_t *out_phys)
{
#ifdef __HOST_TEST__
    void *mem = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (mem == NULL) {
        return NULL;
    }
    memset(mem, 0, PAGE_SIZE);
    if (out_phys != NULL) {
        *out_phys = (uintptr_t)mem;
    }
    return mem;
#else
    addr_t va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (va == 0U) {
        return NULL;
    }
    if (out_phys != NULL) {
        *out_phys = arm64_virt_to_phys((uintptr_t)va);
    }
    return (void *)va;
#endif
}

static void smmu_free_page(void *va)
{
    if (va == NULL) {
        return;
    }
#ifdef __HOST_TEST__
    free(va);
#else
    (void)free_page((addr_t)(uintptr_t)va);
#endif
}

static void smmu_sync_mem(void *va, size_t size)
{
#ifndef __HOST_TEST__
    if (va != NULL && size > 0U) {
        arm64_clean_dcache_poc((uintptr_t)va, size);
    }
#if defined(__aarch64__)
    dsb(ishst);
#endif
#else
    (void)va;
    (void)size;
#endif
}

static uint32_t smmu_read32(uintptr_t off)
{
#ifdef __HOST_TEST__
    if (g_host_mmio.read32 != NULL) {
        return g_host_mmio.read32(off, g_host_mmio.ctx);
    }
    return 0U;
#else
    if (g_smmu.mmio_vbase == NULL) {
        return 0U;
    }
    uint32_t val = *(volatile uint32_t *)(g_smmu.mmio_vbase + off);
#if defined(__aarch64__)
    dsb(sy);
#endif
    return val;
#endif
}

static void smmu_write32(uintptr_t off, uint32_t val)
{
#ifdef __HOST_TEST__
    if (g_host_mmio.write32 != NULL) {
        g_host_mmio.write32(off, val, g_host_mmio.ctx);
    }
#else
    if (g_smmu.mmio_vbase == NULL) {
        return;
    }
    *(volatile uint32_t *)(g_smmu.mmio_vbase + off) = val;
#if defined(__aarch64__)
    dsb(sy);
#endif
#endif
}

static void smmu_write64(uintptr_t off, uint64_t val)
{
#ifdef __HOST_TEST__
    if (g_host_mmio.write64 != NULL) {
        g_host_mmio.write64(off, val, g_host_mmio.ctx);
    }
#else
    if (g_smmu.mmio_vbase == NULL) {
        return;
    }
    *(volatile uint64_t *)(g_smmu.mmio_vbase + off) = val;
#if defined(__aarch64__)
    dsb(sy);
#endif
#endif
}

static int smmu_wait_cr0ack(uint32_t expected)
{
    uint32_t tries = 100000U;
    while (tries-- > 0U) {
        if (smmu_read32(SMMU_CR0ACK) == expected) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

static uint32_t smmu_queue_wrap_mask(uint32_t log2size)
{
    return (1U << (log2size + 1U)) - 1U;
}

static uint32_t smmu_queue_idx_mask(uint32_t log2size)
{
    return (1U << log2size) - 1U;
}

static int smmu_cmdq_push_locked(const struct smmuv3_cmd *cmd)
{
    uint32_t q_wrap_mask = smmu_queue_wrap_mask(g_smmu.cmdq_log2);
    uint32_t q_idx_mask = smmu_queue_idx_mask(g_smmu.cmdq_log2);
    uint32_t cons = smmu_read32(SMMU_CMDQ_CONS) & q_wrap_mask;
    uint32_t next_prod = (g_smmu.cmdq_prod + 1U) & q_wrap_mask;
    uint32_t tries = 200000U;

    /* 队列满判据：索引相等但回绕位相反 */
    while ((next_prod ^ cons) == (1U << g_smmu.cmdq_log2)) {
        if (tries-- == 0U) {
            return -ETIMEDOUT;
        }
        cons = smmu_read32(SMMU_CMDQ_CONS) & q_wrap_mask;
    }

    uint32_t slot = g_smmu.cmdq_prod & q_idx_mask;
    g_smmu.cmdq[slot] = *cmd;
    smmu_sync_mem(&g_smmu.cmdq[slot], sizeof(*cmd));

    g_smmu.cmdq_prod = next_prod;
    smmu_write32(SMMU_CMDQ_PROD, g_smmu.cmdq_prod);
    g_smmu.cmdq_submitted++;
    return 0;
}

static int smmu_cmdq_sync_locked(void)
{
    struct smmuv3_cmd sync_cmd;
    uint32_t q_wrap_mask = smmu_queue_wrap_mask(g_smmu.cmdq_log2);
    uint32_t tries = 400000U;
    int rc;

    if (!g_smmu.hw_present) {
        return 0;
    }

    memset(&sync_cmd, 0, sizeof(sync_cmd));
    sync_cmd.dw[0] = (uint64_t)SMMU_CMD_SYNC;
    rc = smmu_cmdq_push_locked(&sync_cmd);
    if (rc != 0) {
        return rc;
    }

    while (tries-- > 0U) {
        uint32_t cons = smmu_read32(SMMU_CMDQ_CONS) & q_wrap_mask;
        if (cons == (g_smmu.cmdq_prod & q_wrap_mask)) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

static int smmu_issue_cfgi_all_locked(void)
{
    struct smmuv3_cmd cmd;
    int rc;

    memset(&cmd, 0, sizeof(cmd));
    cmd.dw[0] = (uint64_t)SMMU_CMD_CFGI_ALL;
    cmd.dw[1] = 31ULL;
    rc = smmu_cmdq_push_locked(&cmd);
    if (rc != 0) {
        return rc;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_ALL;
    rc = smmu_cmdq_push_locked(&cmd);
    if (rc != 0) {
        return rc;
    }

    return smmu_cmdq_sync_locked();
}

static uint64_t smmu_ips_encode(void)
{
#ifndef __HOST_TEST__
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();
    switch (caps->pa_bits) {
    case 32U: return 0ULL;
    case 36U: return 1ULL;
    case 40U: return 2ULL;
    case 42U: return 3ULL;
    case 44U: return 4ULL;
    case 48U:
    case 52U: return 5ULL; /* 4KB 页表格式下 IHI 0070 最大合法 IPS 为 48-bit (5) */
    default:  return 2ULL;
    }
#else
    return 2ULL;
#endif
}

static void smmu_build_cd(struct smmuv3_cd *cd,
                          uintptr_t root_pt_phys,
                          uint16_t asid)
{
    uint64_t t0sz = (uint64_t)(64 - CONFIG_ARM64_VA_BITS);
    uint64_t ips = smmu_ips_encode();

    memset(cd, 0, sizeof(*cd));
    cd->dw[0] = (t0sz & 0x3FULL) |
                SMMU_CD0_TG0_4K |
                SMMU_CD0_IR0_WBWA |
                SMMU_CD0_OR0_WBWA |
                SMMU_CD0_SH0_ISH |
                SMMU_CD0_EPD1 |
                SMMU_CD0_V |
                (ips << SMMU_CD0_IPS_SHIFT) |
                SMMU_CD0_AFFD |
                SMMU_CD0_WXN |
                SMMU_CD0_AA64 |
                SMMU_CD0_R |
                SMMU_CD0_A |
                SMMU_CD0_ASET |
                ((uint64_t)asid << SMMU_CD0_ASID_SHIFT);
    cd->dw[1] = (uint64_t)root_pt_phys & 0x000FFFFFFFFFFFC0ULL;
    cd->dw[2] = 0ULL;
    /* 必须写入完整 64 位 MAIR0/MAIR1，保证 MT_NORMAL(4) 与 MT_NORMAL_TAGGED(5) 一致 */
    cd->dw[3] = ARM64_MAIR_VALUE;
}

static int smmu_ensure_stream_locked(uint32_t sid)
{
    struct smmu_stream_state *st = &g_smmu.streams[sid];
    uint32_t max_log2;

    if (st->configured) {
        return 0;
    }

    if (!g_smmu.hw_present) {
        st->configured = true;
        st->s1cdmax = SMMU_CD_TABLE_LOG2;
        return 0;
    }

    st->cd_table = (struct smmuv3_cd *)smmu_alloc_page(&st->cd_table_phys);
    if (st->cd_table == NULL) {
        return -ENOMEM;
    }
    smmu_sync_mem(st->cd_table, PAGE_SIZE);

    max_log2 = (g_smmu.ssid_bits >= SMMU_CD_TABLE_LOG2) ?
               SMMU_CD_TABLE_LOG2 : (uint32_t)g_smmu.ssid_bits;
    st->s1cdmax = (uint8_t)max_log2;

    struct smmuv3_ste *ste = &g_smmu.strtab[sid];
    memset(ste, 0, sizeof(*ste));
    ste->dw[0] = SMMU_STE0_V |
                 SMMU_STE0_CFG_S1_TRANS |
                 SMMU_STE0_S1FMT_LINEAR |
                 ((uint64_t)st->cd_table_phys & SMMU_STE0_CTXPTR_MASK);
    ste->dw[1] = ((uint64_t)st->s1cdmax & 0x1FULL) |
                 SMMU_STE1_S1DSS_SSID0 |
                 SMMU_STE1_S1CIR_WB |
                 SMMU_STE1_S1COR_WB |
                 SMMU_STE1_S1CSH_ISH |
                 SMMU_STE1_S1STALLD |
                 SMMU_STE1_SHCFG_INCOMING;
    smmu_sync_mem(ste, sizeof(*ste));

    struct smmuv3_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dw[0] = (uint64_t)SMMU_CMD_CFGI_STE | ((uint64_t)sid << 32);
    cmd.dw[1] = 1ULL;
    if (smmu_cmdq_push_locked(&cmd) != 0 || smmu_cmdq_sync_locked() != 0) {
        return -EIO;
    }

    st->configured = true;
    return 0;
}

static uint32_t smmu_pte_to_prot(uint64_t pte)
{
    uint32_t prot = HAL_VM_PROT_READ;

    if ((pte & ARM64_PTE_AP_RO) == 0ULL) {
        prot |= HAL_VM_PROT_WRITE;
    }
    if ((pte & ARM64_PTE_AP_EL0) != 0ULL) {
        prot |= HAL_VM_PROT_USER;
        if ((pte & ARM64_PTE_UXN) == 0ULL) {
            prot |= HAL_VM_PROT_EXEC;
        }
    } else if ((pte & ARM64_PTE_PXN) == 0ULL) {
        prot |= HAL_VM_PROT_EXEC;
    }
    return prot;
}

int arm64_smmuv3_init(uintptr_t mmio_base, size_t mmio_size)
{
    uint32_t idr0;
    uint32_t idr1;
    uint32_t hw_cmdq_log2;
    uint32_t cr0;
    int rc = 0;

    spin_lock_init(&g_smmu.lock);

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.initialized) {
        spin_unlock_irq(&g_smmu.lock);
        return 0;
    }

#ifdef __HOST_TEST__
    g_smmu.mmio_vbase = (volatile uint8_t *)(uintptr_t)mmio_base;
    (void)mmio_size;
#else
    if (mmio_base != 0U) {
        size_t sz = (mmio_size != 0U) ? mmio_size : ARM64_SMMUV3_DEFAULT_SIZE;
        g_smmu.mmio_vbase = (volatile uint8_t *)arm64_ioremap(mmio_base, sz);
    }
#endif

    idr0 = smmu_read32(SMMU_IDR0);
    if (mmio_base == 0U || idr0 == 0U || idr0 == 0xFFFFFFFFU ||
        (idr0 & SMMU_IDR0_S1P) == 0U ||
        (idr0 & SMMU_IDR0_TTF_AARCH64) == 0U) {
        g_smmu.hw_present = false;
        g_smmu.sid_bits = 6U;
        g_smmu.ssid_bits = 4U;
        g_smmu.max_pasids = SMMU_MAX_PASIDS;
        g_smmu.initialized = true;
        spin_unlock_irq(&g_smmu.lock);
        pr_info("SMMUv3 未检出，已启用 Shadow IOMMU 同步页表隔离模式\n");
        return 0;
    }

    idr1 = smmu_read32(SMMU_IDR1);
    g_smmu.idr0 = idr0;
    g_smmu.idr1 = idr1;
    g_smmu.aidr = smmu_read32(SMMU_AIDR);
    g_smmu.sid_bits = (uint8_t)(idr1 & 0x3FU);
    g_smmu.ssid_bits = (uint8_t)((idr1 >> 6) & 0x1FU);
    g_smmu.max_pasids = (g_smmu.ssid_bits > 0U) ?
                        (1U << ((g_smmu.ssid_bits < SMMU_CD_TABLE_LOG2) ?
                                g_smmu.ssid_bits : SMMU_CD_TABLE_LOG2)) : 1U;

    hw_cmdq_log2 = (idr1 >> 21) & 0x1FU;
    g_smmu.cmdq_log2 = (hw_cmdq_log2 != 0U && hw_cmdq_log2 < SMMU_CMDQ_LOG2SIZE) ?
                       hw_cmdq_log2 : SMMU_CMDQ_LOG2SIZE;

    /* 1. 停机重置 CR0 并等待硬件握手确认 */
    smmu_write32(SMMU_CR0, 0U);
    rc = smmu_wait_cr0ack(0U);
    if (rc != 0) {
        goto fallback_shadow;
    }

    g_smmu.strtab = (struct smmuv3_ste *)smmu_alloc_page(&g_smmu.strtab_phys);
    g_smmu.cmdq = (struct smmuv3_cmd *)smmu_alloc_page(&g_smmu.cmdq_phys);
    g_smmu.evtq = (uint8_t *)smmu_alloc_page(&g_smmu.evtq_phys);
    if (g_smmu.strtab == NULL || g_smmu.cmdq == NULL || g_smmu.evtq == NULL) {
        rc = -ENOMEM;
        goto fallback_shadow;
    }
    smmu_sync_mem(g_smmu.strtab, PAGE_SIZE);
    smmu_sync_mem(g_smmu.cmdq, PAGE_SIZE);
    smmu_sync_mem(g_smmu.evtq, PAGE_SIZE);

    /* 2. 配置缓存一致性、Stream Table、CMDQ 与 EVTQ 基址 */
    smmu_write32(SMMU_CR1, SMMU_CR1_VAL);
    smmu_write32(SMMU_CR2, SMMU_CR2_PTM);

    smmu_write64(SMMU_STRTAB_BASE, (uint64_t)g_smmu.strtab_phys & 0x000FFFFFFFFFFFC0ULL);
    smmu_write32(SMMU_STRTAB_BASE_CFG, SMMU_STRTAB_LOG2SIZE);

    g_smmu.cmdq_prod = 0U;
    smmu_write64(SMMU_CMDQ_BASE,
                 ((uint64_t)g_smmu.cmdq_phys & 0x000FFFFFFFFFFFE0ULL) |
                 (uint64_t)g_smmu.cmdq_log2);
    smmu_write32(SMMU_CMDQ_PROD, 0U);
    smmu_write32(SMMU_CMDQ_CONS, 0U);

    smmu_write64(SMMU_EVTQ_BASE,
                 ((uint64_t)g_smmu.evtq_phys & 0x000FFFFFFFFFFFE0ULL) |
                 (uint64_t)SMMU_EVTQ_LOG2SIZE);
    smmu_write32(SMMU_EVTQ_PROD, 0U);
    smmu_write32(SMMU_EVTQ_CONS, 0U);

    /* 3. 先激活命令/事件队列，再开启 SMMUEN 总开关 */
    cr0 = SMMU_CR0_CMDQEN | SMMU_CR0_EVENTQEN | SMMU_CR0_SMMUEN;
    smmu_write32(SMMU_CR0, cr0);
    rc = smmu_wait_cr0ack(cr0);
    if (rc != 0) {
        goto fallback_shadow;
    }

    /* 4. 使能 EVTQ 与 GERROR 硬件中断门控 */
    smmu_write32(SMMU_IRQ_CTRL, SMMU_IRQ_CTRL_GERROR_IRQEN | SMMU_IRQ_CTRL_EVENTQ_IRQEN);
    {
        uint32_t ack_tries = 10000U;
        while (ack_tries-- > 0U) {
            uint32_t ack = smmu_read32(SMMU_IRQ_CTRLACK);
            if ((ack & (SMMU_IRQ_CTRL_GERROR_IRQEN | SMMU_IRQ_CTRL_EVENTQ_IRQEN)) ==
                (SMMU_IRQ_CTRL_GERROR_IRQEN | SMMU_IRQ_CTRL_EVENTQ_IRQEN)) {
                break;
            }
        }
    }

    g_smmu.hw_present = true;
    rc = smmu_issue_cfgi_all_locked();
    if (rc != 0) {
        goto fallback_shadow;
    }

    g_smmu.initialized = true;
    spin_unlock_irq(&g_smmu.lock);
    pr_info("ARM64 SMMUv3 hardware active (rev=0x%x, sid=%u-bit, ssid=%u-bit, pasids=%u)\n",
            (unsigned int)(g_smmu.aidr & 0xFFU),
            (unsigned int)g_smmu.sid_bits,
            (unsigned int)g_smmu.ssid_bits,
            (unsigned int)g_smmu.max_pasids);
    return 0;

fallback_shadow:
    g_smmu.hw_present = false;
    smmu_free_page(g_smmu.strtab);
    smmu_free_page(g_smmu.cmdq);
    smmu_free_page(g_smmu.evtq);
    g_smmu.strtab = NULL;
    g_smmu.cmdq = NULL;
    g_smmu.evtq = NULL;
    g_smmu.max_pasids = SMMU_MAX_PASIDS;
    g_smmu.initialized = true;
    spin_unlock_irq(&g_smmu.lock);
    pr_info("ARM64 SMMUv3 probe rc=%d，降级至 Shadow IOMMU 模式\n", rc);
    return 0;
}

#ifndef __HOST_TEST__
static void smmu_irq_handler(uint32_t intid, void *ctx)
{
    uint32_t gerr;
    uint32_t gerrn;
    (void)intid;
    (void)ctx;

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.hw_present) {
        gerr = smmu_read32(SMMU_GERROR);
        gerrn = smmu_read32(SMMU_GERRORN);
        if (gerr != gerrn) {
            smmu_write32(SMMU_GERRORN, gerr);
        }
    }
    spin_unlock_irq(&g_smmu.lock);
    (void)nos_iommu_poll_events(16U);
}

static void smmu_setup_irqs_from_fdt(void)
{
    uint32_t evtq_irq = 0U;
    uint32_t gerror_irq = 0U;

    if (g_smmu.irq_registered || !g_smmu.hw_present) {
        return;
    }
    if (arm64_fdt_find_irq("arm,smmu-v3", 0U, 0U, &evtq_irq) == 0 && evtq_irq >= 32U) {
        if (nos_irq_register(evtq_irq, smmu_irq_handler, NULL) == 0) {
            arm64_gic_set_priority(evtq_irq, 0x80U);
            arm64_gic_enable_irq(evtq_irq);
            g_smmu.evtq_irq = evtq_irq;
            g_smmu.irq_registered = true;
        }
    }
    if (arm64_fdt_find_irq("arm,smmu-v3", 0U, 3U, &gerror_irq) == 0 &&
        gerror_irq >= 32U && gerror_irq != evtq_irq) {
        if (nos_irq_register(gerror_irq, smmu_irq_handler, NULL) == 0) {
            arm64_gic_set_priority(gerror_irq, 0x80U);
            arm64_gic_enable_irq(gerror_irq);
            g_smmu.gerror_irq = gerror_irq;
            g_smmu.irq_registered = true;
        }
    }
}
#endif

int arm64_smmuv3_probe_default(void)
{
    uintptr_t base = 0U;
    size_t sz = 0U;
    int rc;

#ifndef __HOST_TEST__
    uint64_t fdt_base = 0ULL;
    uint64_t fdt_sz = 0ULL;

    /* 优先查询运行时引导 FDT，严禁在无 SMMUv3 硬件的平台盲读未映射物理地址触发 SEA */
    if (arm64_fdt_find_reg("arm,smmu-v3", 0U, &fdt_base, &fdt_sz) == 0 &&
        fdt_base != 0ULL) {
        base = (uintptr_t)fdt_base;
        sz = (fdt_sz != 0ULL) ? (size_t)fdt_sz : ARM64_SMMUV3_DEFAULT_SIZE;
    } else if (!arm64_fdt_available()) {
        const struct nos_dt_node *smmu_node = nos_dt_find_compatible("arm,smmu-v3");
        if (smmu_node != NULL) {
            uint32_t r0 = 0U, r1 = 0U, s0 = 0U, s1 = 0U;
            if (nos_dt_read_u32(smmu_node, "reg", 0U, &r0) == 0 &&
                nos_dt_read_u32(smmu_node, "reg", 1U, &r1) == 0) {
                base = ((uintptr_t)r0 << 32) | (uintptr_t)r1;
                sz = ARM64_SMMUV3_DEFAULT_SIZE;
                if (nos_dt_read_u32(smmu_node, "reg", 2U, &s0) == 0 &&
                    nos_dt_read_u32(smmu_node, "reg", 3U, &s1) == 0) {
                    size_t parsed_sz = (size_t)(((uint64_t)s0 << 32) | (uint64_t)s1);
                    if (parsed_sz != 0U) {
                        sz = parsed_sz;
                    }
                }
            }
        }
    }
#endif
    rc = arm64_smmuv3_init(base, sz);
#ifndef __HOST_TEST__
    if (rc == 0) {
        smmu_setup_irqs_from_fdt();
    }
#endif
    return rc;
}

int nos_iommu_bind_space(uint32_t stream_id, uint32_t pasid, struct nos_vm_space *space)
{
    int rc;

    if (space == NULL || !space->initialized || space->root_pt_phys == 0U ||
        stream_id >= SMMU_MAX_STREAMS) {
        return -EINVAL;
    }

    spin_lock_irq(&g_smmu.lock);
    if (!g_smmu.initialized) {
        spin_unlock_irq(&g_smmu.lock);
        return -ENODEV;
    }

    if (pasid >= g_smmu.max_pasids || pasid >= SMMU_MAX_PASIDS) {
        spin_unlock_irq(&g_smmu.lock);
        return -ERANGE;
    }

    rc = smmu_ensure_stream_locked(stream_id);
    if (rc != 0) {
        spin_unlock_irq(&g_smmu.lock);
        return rc;
    }

    struct smmu_stream_state *st = &g_smmu.streams[stream_id];
    uint32_t max_cd = 1U << st->s1cdmax;
    if (pasid >= max_cd) {
        spin_unlock_irq(&g_smmu.lock);
        return -ERANGE;
    }

    if (g_smmu.hw_present && st->cd_table != NULL) {
        struct smmuv3_cmd cmd;
        smmu_build_cd(&st->cd_table[pasid], space->root_pt_phys, space->asid);
        smmu_sync_mem(&st->cd_table[pasid], sizeof(struct smmuv3_cd));

        memset(&cmd, 0, sizeof(cmd));
        cmd.dw[0] = (uint64_t)SMMU_CMD_CFGI_CD |
                    ((uint64_t)pasid << 12) |
                    ((uint64_t)stream_id << 32);
        cmd.dw[1] = 1ULL;
        rc = smmu_cmdq_push_locked(&cmd);
        if (rc == 0) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_ASID | ((uint64_t)space->asid << 48);
            rc = smmu_cmdq_push_locked(&cmd);
        }
        if (rc == 0) {
            rc = smmu_cmdq_sync_locked();
        }
        if (rc != 0) {
            spin_unlock_irq(&g_smmu.lock);
            return rc;
        }
    }

    if (!st->bindings[pasid].active) {
        g_smmu.active_bindings++;
    }
    st->bindings[pasid].active = true;
    st->bindings[pasid].space = space;
    st->bindings[pasid].root_pt_phys = space->root_pt_phys;
    st->bindings[pasid].asid = space->asid;

    spin_unlock_irq(&g_smmu.lock);
    return 0;
}

int nos_iommu_unbind_space(uint32_t stream_id, uint32_t pasid)
{
    struct smmu_stream_state *st;
    uint16_t old_asid;
    int rc = 0;

    if (stream_id >= SMMU_MAX_STREAMS || pasid >= SMMU_MAX_PASIDS) {
        return -EINVAL;
    }

    spin_lock_irq(&g_smmu.lock);
    st = &g_smmu.streams[stream_id];
    if (!g_smmu.initialized || !st->configured || !st->bindings[pasid].active) {
        spin_unlock_irq(&g_smmu.lock);
        return -ENOENT;
    }

    old_asid = st->bindings[pasid].asid;
    st->bindings[pasid].active = false;
    st->bindings[pasid].space = NULL;
    st->bindings[pasid].root_pt_phys = 0U;
    st->bindings[pasid].asid = 0U;
    if (g_smmu.active_bindings > 0U) {
        g_smmu.active_bindings--;
    }

    if (g_smmu.hw_present && st->cd_table != NULL) {
        struct smmuv3_cmd cmd;
        memset(&st->cd_table[pasid], 0, sizeof(struct smmuv3_cd));
        smmu_sync_mem(&st->cd_table[pasid], sizeof(struct smmuv3_cd));

        memset(&cmd, 0, sizeof(cmd));
        cmd.dw[0] = (uint64_t)SMMU_CMD_CFGI_CD |
                    ((uint64_t)pasid << 12) |
                    ((uint64_t)stream_id << 32);
        cmd.dw[1] = 1ULL;
        rc = smmu_cmdq_push_locked(&cmd);
        if (rc == 0) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_ASID | ((uint64_t)old_asid << 48);
            rc = smmu_cmdq_push_locked(&cmd);
        }
        if (rc == 0) {
            rc = smmu_cmdq_sync_locked();
        }
    }

    spin_unlock_irq(&g_smmu.lock);
    return rc;
}

int nos_iommu_translate(uint32_t stream_id,
                        uint32_t pasid,
                        uintptr_t iova,
                        uintptr_t *out_pa,
                        uint32_t *out_prot)
{
    uintptr_t root_phys;
    uintptr_t pa = 0U;
    uint64_t pte = 0ULL;
    const struct arm64_pt_backend *be = NULL;
    int rc;

    if (stream_id >= SMMU_MAX_STREAMS || pasid >= SMMU_MAX_PASIDS) {
        return -EINVAL;
    }

    spin_lock_irq(&g_smmu.lock);
    if (!g_smmu.initialized ||
        !g_smmu.streams[stream_id].configured ||
        !g_smmu.streams[stream_id].bindings[pasid].active) {
        spin_unlock_irq(&g_smmu.lock);
        return -ENOENT;
    }
    root_phys = g_smmu.streams[stream_id].bindings[pasid].root_pt_phys;
    spin_unlock_irq(&g_smmu.lock);

#ifdef __HOST_TEST__
    be = g_host_pt_be;
#endif

    rc = arm64_pt_translate(root_phys, CONFIG_ARM64_VA_BITS, iova, &pa, &pte, be);
    if (rc != 0) {
        return rc;
    }
    /* 只允许 EL0/用户空间授权页进入 SVA DMA 映射，严格屏蔽内核页表位 */
    if ((pte & ARM64_PTE_AP_EL0) == 0ULL) {
        return -EPERM;
    }

    if (out_pa != NULL) {
        *out_pa = pa;
    }
    if (out_prot != NULL) {
        *out_prot = smmu_pte_to_prot(pte);
    }
    return 0;
}

void nos_iommu_tlbi_asid(uint16_t asid)
{
    struct smmuv3_cmd cmd;

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.initialized && g_smmu.hw_present) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_ASID | ((uint64_t)asid << 48);
        if (smmu_cmdq_push_locked(&cmd) == 0) {
            (void)smmu_cmdq_sync_locked();
        }
    }
    spin_unlock_irq(&g_smmu.lock);
}

void nos_iommu_tlbi_va(uint16_t asid, uintptr_t va)
{
    struct smmuv3_cmd cmd;

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.initialized && g_smmu.hw_present) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_VA | ((uint64_t)asid << 48);
        cmd.dw[1] = (uint64_t)va & ~0xFFFULL;
        if (smmu_cmdq_push_locked(&cmd) == 0) {
            (void)smmu_cmdq_sync_locked();
        }
    }
    spin_unlock_irq(&g_smmu.lock);
}

void nos_iommu_tlbi_range(uint16_t asid, uintptr_t va, size_t size)
{
    if (size == 0U || asid == 0U) {
        return;
    }
    if ((size / PAGE_SIZE) > 16U) {
        nos_iommu_tlbi_asid(asid);
        return;
    }

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.initialized && g_smmu.hw_present) {
        size_t off;
        bool pushed = false;
        for (off = 0U; off < size; off += PAGE_SIZE) {
            struct smmuv3_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.dw[0] = (uint64_t)SMMU_CMD_TLBI_NH_VA | ((uint64_t)asid << 48);
            cmd.dw[1] = (uint64_t)(va + off) & ~0xFFFULL;
            if (smmu_cmdq_push_locked(&cmd) != 0) {
                break;
            }
            pushed = true;
        }
        if (pushed) {
            (void)smmu_cmdq_sync_locked();
        }
    }
    spin_unlock_irq(&g_smmu.lock);
}

void nos_iommu_register_fault_cb(nos_iommu_fault_cb_t cb)
{
    uint32_t i;

    spin_lock_irq(&g_smmu.lock);
    if (cb == NULL) {
        for (i = 0U; i < SMMU_MAX_FAULT_CBS; i++) {
            g_fault_cbs[i] = NULL;
        }
        spin_unlock_irq(&g_smmu.lock);
        return;
    }
    for (i = 0U; i < SMMU_MAX_FAULT_CBS; i++) {
        if (g_fault_cbs[i] == cb) {
            spin_unlock_irq(&g_smmu.lock);
            return;
        }
        if (g_fault_cbs[i] == NULL) {
            g_fault_cbs[i] = cb;
            spin_unlock_irq(&g_smmu.lock);
            return;
        }
    }
    spin_unlock_irq(&g_smmu.lock);
}

int nos_iommu_poll_events(uint32_t max_events)
{
    struct nos_iommu_fault_event batch[16];
    nos_iommu_fault_cb_t cbs[SMMU_MAX_FAULT_CBS];
    uint32_t batch_cnt = 0U;
    uint32_t limit = (max_events == 0U || max_events > 16U) ? 16U : max_events;
    uint32_t wrap_mask = smmu_queue_wrap_mask(SMMU_EVTQ_LOG2SIZE);
    uint32_t idx_mask = smmu_queue_idx_mask(SMMU_EVTQ_LOG2SIZE);
    uint32_t i;

    spin_lock_irq(&g_smmu.lock);
    if (g_smmu.initialized && g_smmu.hw_present && g_smmu.evtq != NULL) {
        uint32_t prod = smmu_read32(SMMU_EVTQ_PROD) & wrap_mask;
        uint32_t cons = smmu_read32(SMMU_EVTQ_CONS) & wrap_mask;

        while (prod != cons && batch_cnt < limit) {
            uint32_t slot = cons & idx_mask;
            const uint8_t *raw = g_smmu.evtq + ((size_t)slot * 32U);
            uint64_t d0;
            uint64_t d2;
#ifndef __HOST_TEST__
            arm64_inv_dcache_poc((uintptr_t)raw, 32U);
#endif
            memcpy(&d0, raw, sizeof(d0));
            memcpy(&d2, raw + 16U, sizeof(d2));

            batch[batch_cnt].event_code = (uint32_t)(d0 & 0xFFULL);
            batch[batch_cnt].pasid = (uint32_t)((d0 >> 12) & 0xFFFFFULL);
            batch[batch_cnt].sid = (uint32_t)(d0 >> 32);
            batch[batch_cnt].flags = (uint32_t)((d0 >> 11) & 1ULL); /* SSV */
            batch[batch_cnt].iova = d2;
            batch_cnt++;
            g_smmu.irq_events++;

            cons = (cons + 1U) & wrap_mask;
            smmu_write32(SMMU_EVTQ_CONS, cons);
        }
    }
#ifdef __HOST_TEST__
    while (g_host_inj_cnt > 0U && batch_cnt < limit) {
        batch[batch_cnt++] = g_host_inj_events[g_host_inj_head];
        g_host_inj_head = (g_host_inj_head + 1U) % 16U;
        g_host_inj_cnt--;
        g_smmu.irq_events++;
    }
#endif
    for (i = 0U; i < SMMU_MAX_FAULT_CBS; i++) {
        cbs[i] = g_fault_cbs[i];
    }
    spin_unlock_irq(&g_smmu.lock);

    for (uint32_t e = 0U; e < batch_cnt; e++) {
        for (i = 0U; i < SMMU_MAX_FAULT_CBS; i++) {
            if (cbs[i] != NULL) {
                cbs[i](&batch[e]);
            }
        }
    }
    return (int)batch_cnt;
}

int nos_iommu_get_status(struct nos_iommu_info *out)
{
    if (out == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&g_smmu.lock);
    memset(out, 0, sizeof(*out));
    out->hw_present = g_smmu.hw_present;
    out->arch_rev = (uint8_t)(g_smmu.aidr & 0xFFU);
    out->sid_bits = g_smmu.sid_bits;
    out->ssid_bits = g_smmu.ssid_bits;
    out->max_streams = SMMU_MAX_STREAMS;
    out->max_pasids = g_smmu.max_pasids;
    out->active_bindings = g_smmu.active_bindings;
    out->cmdq_submitted = g_smmu.cmdq_submitted;
    out->irq_events = g_smmu.irq_events;
    spin_unlock_irq(&g_smmu.lock);
    return 0;
}

#ifdef __HOST_TEST__
void smmuv3_host_set_mmio_ops(const struct smmuv3_host_mmio_ops *ops)
{
    if (ops != NULL) {
        g_host_mmio = *ops;
    } else {
        memset(&g_host_mmio, 0, sizeof(g_host_mmio));
    }
}

void smmuv3_host_set_pt_backend(const struct arm64_pt_backend *be)
{
    g_host_pt_be = be;
}

void smmuv3_host_inject_event(const struct nos_iommu_fault_event *evt)
{
    if (evt == NULL) {
        return;
    }
    spin_lock_irq(&g_smmu.lock);
    if (g_host_inj_cnt < 16U) {
        g_host_inj_events[g_host_inj_tail] = *evt;
        g_host_inj_tail = (g_host_inj_tail + 1U) % 16U;
        g_host_inj_cnt++;
    }
    spin_unlock_irq(&g_smmu.lock);
}

void smmuv3_host_reset(void)
{
    uint32_t i;

    for (i = 0U; i < SMMU_MAX_STREAMS; i++) {
        smmu_free_page(g_smmu.streams[i].cd_table);
        g_smmu.streams[i].cd_table = NULL;
    }
    smmu_free_page(g_smmu.strtab);
    smmu_free_page(g_smmu.cmdq);
    smmu_free_page(g_smmu.evtq);
    memset(&g_smmu, 0, sizeof(g_smmu));
    memset(g_fault_cbs, 0, sizeof(g_fault_cbs));
    g_host_inj_head = 0U;
    g_host_inj_tail = 0U;
    g_host_inj_cnt = 0U;
}
#endif
