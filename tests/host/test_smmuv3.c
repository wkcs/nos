/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 SMMUv3 硬件寄存器握手、STE/CD SVA 绑定、CMDQ 同步与跨 Cell 隔离主机单测
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nos/hal_arch.h>
#include <nos/iommu.h>
#include <nos/mm.h>
#include <uapi/nos/errno.h>
#include <asm/fdt_scan.h>
#include <asm/mmu.h>
#include <asm/smmuv3.h>

#define POOL_PAGES 256U
#define POOL_PBASE 0x80000000ULL

struct test_pt_pool {
    uint8_t  mem[POOL_PAGES][4096];
    bool     used[POOL_PAGES];
    uint32_t active;
};

static struct test_pt_pool g_pool;

static uintptr_t pool_alloc(void *ctx)
{
    struct test_pt_pool *p = (struct test_pt_pool *)ctx;
    for (uint32_t i = 0U; i < POOL_PAGES; i++) {
        if (!p->used[i]) {
            p->used[i] = true;
            p->active++;
            memset(p->mem[i], 0, 4096U);
            return (uintptr_t)(POOL_PBASE + (uint64_t)i * 4096ULL);
        }
    }
    return 0U;
}

static void pool_free(void *ctx, uintptr_t pa)
{
    struct test_pt_pool *p = (struct test_pt_pool *)ctx;
    assert(pa >= POOL_PBASE);
    uint64_t idx = (pa - POOL_PBASE) / 4096ULL;
    assert(idx < POOL_PAGES && p->used[idx]);
    p->used[idx] = false;
    p->active--;
}

static void *pool_p2v(void *ctx, uintptr_t pa)
{
    struct test_pt_pool *p = (struct test_pt_pool *)ctx;
    assert(pa >= POOL_PBASE);
    uint64_t off = pa - POOL_PBASE;
    assert(off + 4096ULL <= sizeof(p->mem));
    return (void *)((uint8_t *)p->mem + off);
}

static const struct arm64_pt_backend g_be = {
    .alloc_pt_page   = pool_alloc,
    .free_pt_page    = pool_free,
    .phys_to_pt_virt = pool_p2v,
    .ctx             = &g_pool,
};

struct mock_smmu_hw {
    uint32_t cr0;
    uint32_t cr0ack;
    uint32_t cr1;
    uint32_t cr2;
    uint64_t strtab_base;
    uint32_t strtab_cfg;
    uint64_t cmdq_base;
    uint32_t cmdq_prod;
    uint32_t cmdq_cons;
    uint64_t evtq_base;
    uint32_t op_mask;
    uint32_t cmds_executed;
};

static struct mock_smmu_hw g_hw;

static uint32_t mock_read32(uintptr_t off, void *ctx)
{
    struct mock_smmu_hw *hw = (struct mock_smmu_hw *)ctx;
    switch (off) {
    case SMMU_IDR0:
        return SMMU_IDR0_S1P | SMMU_IDR0_TTF_AARCH64 | SMMU_IDR0_ASID16 | SMMU_IDR0_BTM;
    case SMMU_IDR1:
        return (6U << 0) | (4U << 6) | (7U << 16) | (8U << 21);
    case SMMU_AIDR:
        return 0x02U;
    case SMMU_CR0:
        return hw->cr0;
    case SMMU_CR0ACK:
        return hw->cr0ack;
    case SMMU_CMDQ_PROD:
        return hw->cmdq_prod;
    case SMMU_CMDQ_CONS:
        return hw->cmdq_cons;
    default:
        return 0U;
    }
}

static void mock_write32(uintptr_t off, uint32_t val, void *ctx)
{
    struct mock_smmu_hw *hw = (struct mock_smmu_hw *)ctx;
    switch (off) {
    case SMMU_CR0:
        hw->cr0 = val;
        hw->cr0ack = val;
        break;
    case SMMU_CR1:
        hw->cr1 = val;
        break;
    case SMMU_CR2:
        hw->cr2 = val;
        break;
    case SMMU_STRTAB_BASE_CFG:
        hw->strtab_cfg = val;
        break;
    case SMMU_CMDQ_PROD:
        hw->cmdq_prod = val;
        if ((hw->cr0 & SMMU_CR0_CMDQEN) != 0U && hw->cmdq_base != 0ULL) {
            uint32_t log2 = (uint32_t)(hw->cmdq_base & 0x1FULL);
            uint32_t wrap_mask = (1U << (log2 + 1U)) - 1U;
            uint32_t idx_mask = (1U << log2) - 1U;
            struct smmuv3_cmd *q = (struct smmuv3_cmd *)(uintptr_t)(hw->cmdq_base & 0x000FFFFFFFFFFFE0ULL);
            while (hw->cmdq_cons != (hw->cmdq_prod & wrap_mask)) {
                uint32_t slot = hw->cmdq_cons & idx_mask;
                uint32_t op = (uint32_t)(q[slot].dw[0] & 0xFFULL);
                if (op < 32U) {
                    hw->op_mask |= (1U << op);
                } else if (op == SMMU_CMD_SYNC) {
                    hw->op_mask |= (1U << 31);
                }
                hw->cmds_executed++;
                hw->cmdq_cons = (hw->cmdq_cons + 1U) & wrap_mask;
            }
        }
        break;
    case SMMU_CMDQ_CONS:
        hw->cmdq_cons = val;
        break;
    default:
        break;
    }
}

static void mock_write64(uintptr_t off, uint64_t val, void *ctx)
{
    struct mock_smmu_hw *hw = (struct mock_smmu_hw *)ctx;
    switch (off) {
    case SMMU_STRTAB_BASE:
        hw->strtab_base = val;
        break;
    case SMMU_CMDQ_BASE:
        hw->cmdq_base = val;
        break;
    case SMMU_EVTQ_BASE:
        hw->evtq_base = val;
        break;
    default:
        break;
    }
}

static void setup_space(struct nos_vm_space *sp, uint16_t asid)
{
    memset(sp, 0, sizeof(*sp));
    sp->root_pt_phys = pool_alloc(&g_pool);
    assert(sp->root_pt_phys != 0U);
    sp->asid = asid;
    sp->initialized = true;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void test_fdt_scanner(void)
{
    uint8_t blob[512];
    uint32_t off = 40U;
    uint32_t off_strings;
    uint32_t size_struct;
    uint64_t base = 0ULL;
    uint64_t sz = 0ULL;

    memset(blob, 0, sizeof(blob));
    put_be32(&blob[off], 1U); off += 4U;                   /* FDT_BEGIN_NODE "" */
    off += 4U;
    put_be32(&blob[off], 1U); off += 4U;                   /* FDT_BEGIN_NODE "smmuv3@9050000" */
    memcpy(&blob[off], "smmuv3@9050000", 15U); off += 16U;
    put_be32(&blob[off], 3U);                              /* FDT_PROP compatible */
    put_be32(&blob[off + 4U], 12U);
    put_be32(&blob[off + 8U], 0U);
    off += 12U;
    memcpy(&blob[off], "arm,smmu-v3", 12U); off += 12U;
    put_be32(&blob[off], 3U);                              /* FDT_PROP reg */
    put_be32(&blob[off + 4U], 16U);
    put_be32(&blob[off + 8U], 11U);
    off += 12U;
    put_be32(&blob[off], 0U); put_be32(&blob[off + 4U], 0x09050000U);
    put_be32(&blob[off + 8U], 0U); put_be32(&blob[off + 12U], 0x00020000U);
    off += 16U;
    put_be32(&blob[off], 2U); off += 4U;                   /* FDT_END_NODE */
    put_be32(&blob[off], 2U); off += 4U;                   /* FDT_END_NODE */
    put_be32(&blob[off], 9U); off += 4U;                   /* FDT_END */

    size_struct = off - 40U;
    off_strings = off;
    memcpy(&blob[off], "compatible\0reg\0", 15U);
    off += 15U;

    put_be32(&blob[0], ARM64_FDT_MAGIC);
    put_be32(&blob[4], off);
    put_be32(&blob[8], 40U);
    put_be32(&blob[12], off_strings);
    put_be32(&blob[16], 40U);
    put_be32(&blob[20], 17U);
    put_be32(&blob[24], 16U);
    put_be32(&blob[32], 15U);
    put_be32(&blob[36], size_struct);

    assert(arm64_fdt_blob_find_reg(blob, sizeof(blob), "arm,smmu-v3", 0U, &base, &sz) == 0);
    assert(base == 0x09050000ULL && sz == 0x00020000ULL);
    assert(arm64_fdt_blob_find_reg(blob, sizeof(blob), "arm,missing", 0U, &base, &sz) == -ENOENT);
}

int main(void)
{
    struct nos_iommu_info info;
    struct nos_vm_space space_a;
    struct nos_vm_space space_b;
    uintptr_t pa = 0U;
    uint32_t prot = 0U;

    test_fdt_scanner();

    memset(&g_pool, 0, sizeof(g_pool));
    setup_space(&space_a, 10U);
    setup_space(&space_b, 20U);

    assert(arm64_pt_map_range(space_a.root_pt_phys, 39, 0x400000UL, 0x90001000UL, 4096U,
                              HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, true, &g_be) == 0);
    assert(arm64_pt_map_range(space_a.root_pt_phys, 39, 0x410000UL, 0x90002000UL, 4096U,
                              HAL_VM_PROT_USER | HAL_VM_PROT_READ, true, &g_be) == 0);
    assert(arm64_pt_map_range(space_a.root_pt_phys, 39, 0x500000UL, 0x90003000UL, 4096U,
                              HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, false, &g_be) == 0);
    assert(arm64_pt_map_range(space_b.root_pt_phys, 39, 0x600000UL, 0x91001000UL, 4096U,
                              HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, true, &g_be) == 0);

    smmuv3_host_set_pt_backend(&g_be);

    /* 1. 验证无硬件时自动降级 Shadow IOMMU 模式与隔离语义 */
    smmuv3_host_reset();
    smmuv3_host_set_mmio_ops(NULL);
    assert(arm64_smmuv3_init(0x09050000UL, 0x20000UL) == 0);
    assert(nos_iommu_get_status(&info) == 0);
    assert(!info.hw_present);
    assert(nos_iommu_bind_space(1U, 0U, &space_a) == 0);
    assert(nos_iommu_translate(1U, 0U, 0x400080UL, &pa, &prot) == 0);
    assert(pa == 0x90001080UL && (prot & HAL_VM_PROT_WRITE) != 0U);
    assert(nos_iommu_translate(1U, 0U, 0x500000UL, &pa, &prot) == -EPERM);
    assert(nos_iommu_translate(1U, 0U, 0x600000UL, &pa, &prot) == -ENOENT);

    /* 2. 验证真实硬件模式：CR0 握手、STE/CD 写入（全 64 位 MAIR + EPD1=1）、CMDQ 同步及多 Stream 隔离 */
    memset(&g_hw, 0, sizeof(g_hw));
    struct smmuv3_host_mmio_ops ops = {
        .read32 = mock_read32,
        .write32 = mock_write32,
        .write64 = mock_write64,
        .ctx = &g_hw,
    };
    smmuv3_host_reset();
    smmuv3_host_set_mmio_ops(&ops);

    assert(arm64_smmuv3_init(0x09050000UL, 0x20000UL) == 0);
    assert(nos_iommu_get_status(&info) == 0);
    assert(info.hw_present);
    assert(info.arch_rev == 0x02U);
    assert(info.sid_bits == 6U && info.ssid_bits == 4U && info.max_pasids == 16U);
    assert((g_hw.cr0ack & (SMMU_CR0_SMMUEN | SMMU_CR0_CMDQEN | SMMU_CR0_EVENTQEN)) ==
           (SMMU_CR0_SMMUEN | SMMU_CR0_CMDQEN | SMMU_CR0_EVENTQEN));
    assert((g_hw.op_mask & (1U << SMMU_CMD_CFGI_ALL)) != 0U);
    assert((g_hw.op_mask & (1U << SMMU_CMD_TLBI_NH_ALL)) != 0U);
    assert((g_hw.op_mask & (1U << 31)) != 0U);

    assert(nos_iommu_bind_space(1U, 2U, &space_a) == 0);
    assert(nos_iommu_bind_space(2U, 0U, &space_b) == 0);
    assert(nos_iommu_get_status(&info) == 0);
    assert(info.active_bindings == 2U);
    assert((g_hw.op_mask & (1U << SMMU_CMD_CFGI_STE)) != 0U);
    assert((g_hw.op_mask & (1U << SMMU_CMD_CFGI_CD)) != 0U);
    assert((g_hw.op_mask & (1U << SMMU_CMD_TLBI_NH_ASID)) != 0U);

    /* 核验硬件 STE[1] 与 CD[2] 描述符位域严格符合 ARM IHI 0070 */
    struct smmuv3_ste *stes = (struct smmuv3_ste *)(uintptr_t)(g_hw.strtab_base & 0x000FFFFFFFFFFFC0ULL);
    assert((stes[1].dw[0] & SMMU_STE0_V) != 0ULL);
    assert((stes[1].dw[0] & (7ULL << 1)) == SMMU_STE0_CFG_S1_TRANS);
    assert((stes[1].dw[1] & (3ULL << 6)) == SMMU_STE1_S1DSS_SSID0);
    struct smmuv3_cd *cds = (struct smmuv3_cd *)(uintptr_t)(stes[1].dw[0] & SMMU_STE0_CTXPTR_MASK);
    assert((cds[2].dw[0] & SMMU_CD0_V) != 0ULL);
    assert((cds[2].dw[0] & SMMU_CD0_EPD1) != 0ULL);
    assert((cds[2].dw[0] & SMMU_CD0_AA64) != 0ULL);
    assert((uint16_t)(cds[2].dw[0] >> SMMU_CD0_ASID_SHIFT) == 10U);
    assert(cds[2].dw[1] == (uint64_t)space_a.root_pt_phys);
    assert(cds[2].dw[3] == ARM64_MAIR_VALUE);

    /* 验证硬件翻译、只读权限隔离及跨 Cell/未授权边界拦截 */
    assert(nos_iommu_translate(1U, 2U, 0x400100UL, &pa, &prot) == 0);
    assert(pa == 0x90001100UL && (prot & HAL_VM_PROT_WRITE) != 0U);
    assert(nos_iommu_translate(1U, 2U, 0x410200UL, &pa, &prot) == 0);
    assert(pa == 0x90002200UL && (prot & HAL_VM_PROT_WRITE) == 0U);
    assert(nos_iommu_translate(1U, 2U, 0x500000UL, &pa, &prot) == -EPERM);
    assert(nos_iommu_translate(1U, 2U, 0x600000UL, &pa, &prot) == -ENOENT);
    assert(nos_iommu_translate(1U, 3U, 0x400000UL, &pa, &prot) == -ENOENT);
    assert(nos_iommu_translate(2U, 0U, 0x600040UL, &pa, &prot) == 0);
    assert(pa == 0x91001040UL);
    assert(nos_iommu_translate(2U, 0U, 0x400000UL, &pa, &prot) == -ENOENT);

    nos_iommu_tlbi_va(10U, 0x400000UL);
    assert((g_hw.op_mask & (1U << SMMU_CMD_TLBI_NH_VA)) != 0U);

    assert(nos_iommu_unbind_space(1U, 2U) == 0);
    assert((cds[2].dw[0] & SMMU_CD0_V) == 0ULL);
    assert(nos_iommu_translate(1U, 2U, 0x400000UL, &pa, &prot) == -ENOENT);

    assert(nos_iommu_unbind_space(2U, 0U) == 0);
    assert(nos_iommu_get_status(&info) == 0 && info.active_bindings == 0U);

    smmuv3_host_reset();
    arm64_pt_destroy(space_a.root_pt_phys, 39, &g_be);
    arm64_pt_destroy(space_b.root_pt_phys, 39, &g_be);
    assert(g_pool.active == 0U);

    printf("PASS: test_smmuv3 (HW CR0/CMDQ/STE/CD SVA, full MAIR, PASID isolation, Shadow fallback)\n");
    return 0;
}
