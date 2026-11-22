/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Stage-1 硬件页表引擎（39-bit 3 级 / 48-bit 4 级）
 * 支持 2MB Block、64KB PTE_CONT、W^X 硬件强制、Break-Before-Make 与 16-bit ASID 世代回绕。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <asm/mmu.h>

#ifndef __HOST_TEST__
#include <asm/barrier.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#endif

#define ARM64_PT_ENTRIES        512U
#define ARM64_PT_INDEX_MASK     0x1FFULL
#define ARM64_ASID_MAX          65535U

/* 16-bit ASID 世代分配状态 */
static uint32_t g_asid_cursor = 1U;
static uint64_t g_asid_epoch = 1ULL;

/* ---------------- 内置 TLB 与内存屏障原语 ---------------- */

static inline void arm64_tlbi_va(uintptr_t vaddr)
{
#if defined(__aarch64__) && !defined(__HOST_TEST__)
    uint64_t arg = ((uint64_t)vaddr >> PAGE_SHIFT) & 0x00000FFFFFFFFFFFULL;
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi vaae1is, %0\n\t"
                     "dsb ish\n\t"
                     "isb"
                     :: "r"(arg) : "memory");
#else
    (void)vaddr;
#endif
}

static inline void arm64_tlbi_all(void)
{
#if defined(__aarch64__) && !defined(__HOST_TEST__)
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi vmalle1is\n\t"
                     "dsb ish\n\t"
                     "isb"
                     ::: "memory");
#endif
}

static inline void arm64_write_pte(volatile uint64_t *slot, uint64_t val)
{
    *slot = val;
#if defined(__aarch64__) && !defined(__HOST_TEST__)
    dsb(ishst);
#endif
}

/* Break-Before-Make：清零旧项、刷 TLB 再写新项，防止微架构多命中异常 */
static inline void arm64_bbm_update_pte(volatile uint64_t *slot,
                                        uintptr_t vaddr,
                                        uint64_t new_val)
{
    if ((*slot & ARM64_PTE_VALID) != 0ULL) {
        arm64_write_pte(slot, 0ULL);
        arm64_tlbi_va(vaddr);
    }
    arm64_write_pte(slot, new_val);
}

/* ---------------- 默认真机页表后端 ---------------- */

#ifndef __HOST_TEST__
/* 早期静态页表池（在 mm_init() 未就绪前用于内核 W^X 细化与早期 ioremap） */
#define ARM64_EARLY_PT_POOL_PAGES 48U
static uint8_t g_early_pt_pool[ARM64_EARLY_PT_POOL_PAGES][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t g_early_pt_used;
static bool g_mm_ready;

static uintptr_t default_alloc_pt_page(void *ctx)
{
    (void)ctx;
    if (g_mm_ready) {
        addr_t va = alloc_page(GFP_KERNEL | GFP_ZERO);
        if (va != 0U) {
            return arm64_virt_to_phys((uintptr_t)va);
        }
    }
    if (g_early_pt_used < ARM64_EARLY_PT_POOL_PAGES) {
        void *va = g_early_pt_pool[g_early_pt_used++];
        memset(va, 0, PAGE_SIZE);
        return arm64_virt_to_phys((uintptr_t)va);
    }
    return 0U;
}

static void default_free_pt_page(void *ctx, uintptr_t pt_phys)
{
    uintptr_t pool_start = arm64_virt_to_phys((uintptr_t)&g_early_pt_pool[0]);
    uintptr_t pool_end = pool_start + (uintptr_t)sizeof(g_early_pt_pool);
    (void)ctx;
    if (pt_phys >= pool_start && pt_phys < pool_end) {
        return;
    }
    if (g_mm_ready && pt_phys != 0U) {
        free_page((addr_t)arm64_phys_to_virt(pt_phys));
    }
}

static void *default_phys_to_pt_virt(void *ctx, uintptr_t phys)
{
    (void)ctx;
    return (void *)arm64_phys_to_virt(phys);
}

static const struct arm64_pt_backend g_default_be = {
    .alloc_pt_page   = default_alloc_pt_page,
    .free_pt_page    = default_free_pt_page,
    .phys_to_pt_virt = default_phys_to_pt_virt,
    .ctx             = NULL,
};
#else
static uintptr_t host_noop_alloc(void *ctx) { (void)ctx; return 0U; }
static void host_noop_free(void *ctx, uintptr_t p) { (void)ctx; (void)p; }
static void *host_noop_p2v(void *ctx, uintptr_t p) { (void)ctx; return (void *)p; }
static const struct arm64_pt_backend g_default_be = {
    .alloc_pt_page   = host_noop_alloc,
    .free_pt_page    = host_noop_free,
    .phys_to_pt_virt = host_noop_p2v,
    .ctx             = NULL,
};
#endif

static const struct arm64_pt_backend *resolve_backend(const struct arm64_pt_backend *be)
{
    return (be != NULL) ? be : &g_default_be;
}

/* ---------------- PTE 叶节点构造与 2MB 块裂解 ---------------- */

static uint32_t arm64_pte_to_prot(uint64_t pte)
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
    if (((pte >> 2) & 7ULL) == MT_DEVICE_nGnRnE ||
        ((pte >> 2) & 7ULL) == MT_DEVICE_nGnRE) {
        prot |= HAL_VM_PROT_DEV;
    }
    return prot;
}

uint64_t arm64_pte_make_leaf(uintptr_t paddr,
                             uint32_t prot_flags,
                             bool is_block,
                             bool contiguous,
                             bool non_global)
{
    bool is_user  = (prot_flags & HAL_VM_PROT_USER) != 0U;
    bool is_write = (prot_flags & HAL_VM_PROT_WRITE) != 0U;
    bool is_exec  = (prot_flags & HAL_VM_PROT_EXEC) != 0U;
    bool is_dev   = (prot_flags & HAL_VM_PROT_DEV) != 0U;

    uint64_t desc = ((uint64_t)paddr & ARM64_PTE_ADDR_MASK) |
                    ARM64_PTE_VALID |
                    (is_block ? ARM64_PTE_BLOCK : ARM64_PTE_PAGE) |
                    ARM64_PTE_AF |
                    ARM64_PTE_PXN |
                    ARM64_PTE_UXN;

    if (is_dev) {
        desc |= ARM64_PTE_ATTRINDX(MT_DEVICE_nGnRnE);
    } else {
        desc |= ARM64_PTE_ATTRINDX(MT_NORMAL) | ARM64_PTE_SH_INNER;
    }

    if (is_user) {
        desc |= ARM64_PTE_AP_EL0;
    }
    if (!is_write) {
        desc |= ARM64_PTE_AP_RO;
    }

    /* 硬件执行隔离：用户态只开 UXN=0，内核态只开 PXN=0 */
    if (is_exec) {
        if (is_user) {
            desc &= ~ARM64_PTE_UXN;
        } else {
            desc &= ~ARM64_PTE_PXN;
        }
    }

    if (contiguous && !is_block) {
        desc |= ARM64_PTE_CONT;
    }
    if (non_global) {
        desc |= ARM64_PTE_NG;
    }

    return desc;
}

static uint64_t *pt_get_l1(uintptr_t root_phys,
                           int va_bits,
                           uintptr_t vaddr,
                           bool alloc,
                           const struct arm64_pt_backend *be)
{
    uint64_t *root_va = (uint64_t *)be->phys_to_pt_virt(be->ctx, root_phys);
    if (root_va == NULL) {
        return NULL;
    }
    if (va_bits <= 39) {
        return root_va;
    }

    uint32_t idx0 = (uint32_t)(((uint64_t)vaddr >> 39) & ARM64_PT_INDEX_MASK);
    if ((root_va[idx0] & ARM64_PTE_VALID) == 0ULL) {
        if (!alloc) {
            return NULL;
        }
        uintptr_t l1_pa = be->alloc_pt_page(be->ctx);
        if (l1_pa == 0U) {
            return NULL;
        }
        arm64_write_pte(&root_va[idx0],
                        ((uint64_t)l1_pa & ARM64_PTE_ADDR_MASK) |
                        ARM64_PTE_VALID | ARM64_PTE_TABLE);
    }
    return (uint64_t *)be->phys_to_pt_virt(be->ctx,
                                            (uintptr_t)(root_va[idx0] & ARM64_PTE_ADDR_MASK));
}

static uint64_t *pt_get_l2(uint64_t *l1_va,
                           uintptr_t vaddr,
                           bool alloc,
                           const struct arm64_pt_backend *be)
{
    uint32_t idx1 = (uint32_t)(((uint64_t)vaddr >> 30) & ARM64_PT_INDEX_MASK);
    if (l1_va == NULL) {
        return NULL;
    }
    if ((l1_va[idx1] & ARM64_PTE_VALID) == 0ULL) {
        if (!alloc) {
            return NULL;
        }
        uintptr_t l2_pa = be->alloc_pt_page(be->ctx);
        if (l2_pa == 0U) {
            return NULL;
        }
        arm64_write_pte(&l1_va[idx1],
                        ((uint64_t)l2_pa & ARM64_PTE_ADDR_MASK) |
                        ARM64_PTE_VALID | ARM64_PTE_TABLE);
    } else if ((l1_va[idx1] & ARM64_PTE_TABLE) == 0ULL) {
        /* 暂不在此拆分 1GB 巨块 */
        return NULL;
    }
    return (uint64_t *)be->phys_to_pt_virt(be->ctx,
                                            (uintptr_t)(l1_va[idx1] & ARM64_PTE_ADDR_MASK));
}

static int pt_split_2m_block(volatile uint64_t *l2_entry,
                             uintptr_t block_vaddr,
                             const struct arm64_pt_backend *be)
{
    uint64_t old_desc = *l2_entry;
    uintptr_t base_pa = (uintptr_t)(old_desc & ARM64_PTE_ADDR_MASK);
    uint32_t prot = arm64_pte_to_prot(old_desc);
    bool non_global = (old_desc & ARM64_PTE_NG) != 0ULL;
    uintptr_t l3_pa = be->alloc_pt_page(be->ctx);
    uint64_t *l3_va;

    if (l3_pa == 0U) {
        return -ENOMEM;
    }
    l3_va = (uint64_t *)be->phys_to_pt_virt(be->ctx, l3_pa);
    for (uint32_t i = 0U; i < ARM64_PT_ENTRIES; i++) {
        l3_va[i] = arm64_pte_make_leaf(base_pa + ((uintptr_t)i << PAGE_SHIFT),
                                       prot, false, false, non_global);
    }

    arm64_bbm_update_pte(l2_entry,
                         block_vaddr & ARM64_BLOCK_2M_MASK,
                         ((uint64_t)l3_pa & ARM64_PTE_ADDR_MASK) |
                         ARM64_PTE_VALID | ARM64_PTE_TABLE);
    return 0;
}

int arm64_pt_map_range(uintptr_t root_phys,
                       int va_bits,
                       uintptr_t vaddr,
                       uintptr_t paddr,
                       size_t size,
                       uint32_t prot_flags,
                       bool non_global,
                       const struct arm64_pt_backend *be)
{
    const struct arm64_pt_backend *ops = resolve_backend(be);
    size_t offset = 0U;

    if (root_phys == 0U || size == 0U) {
        return -EINVAL;
    }
    if (((vaddr | paddr | size) & (PAGE_SIZE - 1UL)) != 0UL) {
        return -EINVAL;
    }
    /* W^X 安全铁律：严禁任何页同时具备可写与可执行权限 */
    if ((prot_flags & (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) ==
        (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) {
        return -EINVAL;
    }

    while (offset < size) {
        uintptr_t cur_va = vaddr + offset;
        uintptr_t cur_pa = paddr + offset;
        size_t remain = size - offset;

        uint64_t *l1 = pt_get_l1(root_phys, va_bits, cur_va, true, ops);
        if (l1 == NULL) {
            return -ENOMEM;
        }
        uint64_t *l2 = pt_get_l2(l1, cur_va, true, ops);
        if (l2 == NULL) {
            return -ENOMEM;
        }

        uint32_t idx2 = (uint32_t)(((uint64_t)cur_va >> 21) & ARM64_PT_INDEX_MASK);

        /* 优先使用 2MB Block 降低 TLB 压力（支持权重与 Physmap 大块） */
        if (remain >= ARM64_BLOCK_2M_SIZE &&
            ((cur_va | cur_pa) & (ARM64_BLOCK_2M_SIZE - 1UL)) == 0UL &&
            (l2[idx2] & ARM64_PTE_VALID) == 0ULL) {
            uint64_t blk = arm64_pte_make_leaf(cur_pa, prot_flags, true, false, non_global);
            arm64_write_pte(&l2[idx2], blk);
            offset += ARM64_BLOCK_2M_SIZE;
            continue;
        }

        /* 确保 L2 指向 L3 表（若已有 2MB Block 则先裂解） */
        if ((l2[idx2] & ARM64_PTE_VALID) != 0ULL &&
            (l2[idx2] & ARM64_PTE_TABLE) == 0ULL) {
            int split_rc = pt_split_2m_block(&l2[idx2], cur_va, ops);
            if (split_rc < 0) {
                return split_rc;
            }
        } else if ((l2[idx2] & ARM64_PTE_VALID) == 0ULL) {
            uintptr_t l3_pa = ops->alloc_pt_page(ops->ctx);
            if (l3_pa == 0U) {
                return -ENOMEM;
            }
            arm64_write_pte(&l2[idx2],
                            ((uint64_t)l3_pa & ARM64_PTE_ADDR_MASK) |
                            ARM64_PTE_VALID | ARM64_PTE_TABLE);
        }

        uint64_t *l3 = (uint64_t *)ops->phys_to_pt_virt(
            ops->ctx, (uintptr_t)(l2[idx2] & ARM64_PTE_ADDR_MASK));
        uint32_t idx3 = (uint32_t)(((uint64_t)cur_va >> 12) & ARM64_PT_INDEX_MASK);

        /* 64KB 对齐连续映射（16 个 4KB 页）自动置位 ARM64_PTE_CONT */
        if (remain >= ARM64_CONT_64K_SIZE &&
            ((cur_va | cur_pa) & (ARM64_CONT_64K_SIZE - 1UL)) == 0UL &&
            (idx3 + ARM64_CONT_64K_PAGES) <= ARM64_PT_ENTRIES) {
            for (uint32_t i = 0U; i < ARM64_CONT_64K_PAGES; i++) {
                uint64_t leaf = arm64_pte_make_leaf(cur_pa + ((uintptr_t)i << PAGE_SHIFT),
                                                    prot_flags, false, true, non_global);
                arm64_bbm_update_pte(&l3[idx3 + i],
                                     cur_va + ((uintptr_t)i << PAGE_SHIFT),
                                     leaf);
            }
            offset += ARM64_CONT_64K_SIZE;
            continue;
        }

        uint64_t leaf = arm64_pte_make_leaf(cur_pa, prot_flags, false, false, non_global);
        arm64_bbm_update_pte(&l3[idx3], cur_va, leaf);
        offset += PAGE_SIZE;
    }

    return 0;
}

int arm64_pt_unmap_range(uintptr_t root_phys,
                         int va_bits,
                         uintptr_t vaddr,
                         size_t size,
                         bool flush_tlb,
                         const struct arm64_pt_backend *be)
{
    const struct arm64_pt_backend *ops = resolve_backend(be);
    size_t offset = 0U;

    if (root_phys == 0U || size == 0U ||
        ((vaddr | size) & (PAGE_SIZE - 1UL)) != 0UL) {
        return -EINVAL;
    }

    while (offset < size) {
        uintptr_t cur_va = vaddr + offset;
        size_t remain = size - offset;
        uint64_t *l1 = pt_get_l1(root_phys, va_bits, cur_va, false, ops);
        if (l1 == NULL) {
            offset += ARM64_BLOCK_2M_SIZE;
            continue;
        }
        uint64_t *l2 = pt_get_l2(l1, cur_va, false, ops);
        if (l2 == NULL) {
            offset += PAGE_SIZE;
            continue;
        }
        uint32_t idx2 = (uint32_t)(((uint64_t)cur_va >> 21) & ARM64_PT_INDEX_MASK);
        if ((l2[idx2] & ARM64_PTE_VALID) == 0ULL) {
            offset += PAGE_SIZE;
            continue;
        }

        if ((l2[idx2] & ARM64_PTE_TABLE) == 0ULL) {
            if (remain >= ARM64_BLOCK_2M_SIZE &&
                (cur_va & (ARM64_BLOCK_2M_SIZE - 1UL)) == 0UL) {
                arm64_write_pte(&l2[idx2], 0ULL);
                if (flush_tlb) {
                    arm64_tlbi_va(cur_va);
                }
                offset += ARM64_BLOCK_2M_SIZE;
                continue;
            }
            int rc = pt_split_2m_block(&l2[idx2], cur_va, ops);
            if (rc < 0) {
                return rc;
            }
        }

        uint64_t *l3 = (uint64_t *)ops->phys_to_pt_virt(
            ops->ctx, (uintptr_t)(l2[idx2] & ARM64_PTE_ADDR_MASK));
        uint32_t idx3 = (uint32_t)(((uint64_t)cur_va >> 12) & ARM64_PT_INDEX_MASK);
        if ((l3[idx3] & ARM64_PTE_VALID) != 0ULL) {
            arm64_write_pte(&l3[idx3], 0ULL);
            if (flush_tlb) {
                arm64_tlbi_va(cur_va);
            }
        }
        offset += PAGE_SIZE;
    }

    return 0;
}

int arm64_pt_mprotect(uintptr_t root_phys,
                      int va_bits,
                      uintptr_t vaddr,
                      size_t size,
                      uint32_t new_prot,
                      bool non_global,
                      const struct arm64_pt_backend *be)
{
    const struct arm64_pt_backend *ops = resolve_backend(be);
    size_t offset = 0U;

    if (root_phys == 0U || size == 0U ||
        ((vaddr | size) & (PAGE_SIZE - 1UL)) != 0UL) {
        return -EINVAL;
    }
    if ((new_prot & (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) ==
        (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) {
        return -EINVAL;
    }

    while (offset < size) {
        uintptr_t cur_va = vaddr + offset;
        size_t remain = size - offset;
        uint64_t *l1 = pt_get_l1(root_phys, va_bits, cur_va, false, ops);
        if (l1 == NULL) {
            return -ENOENT;
        }
        uint64_t *l2 = pt_get_l2(l1, cur_va, false, ops);
        if (l2 == NULL) {
            return -ENOENT;
        }
        uint32_t idx2 = (uint32_t)(((uint64_t)cur_va >> 21) & ARM64_PT_INDEX_MASK);
        if ((l2[idx2] & ARM64_PTE_VALID) == 0ULL) {
            return -ENOENT;
        }

        if ((l2[idx2] & ARM64_PTE_TABLE) == 0ULL) {
            if (remain >= ARM64_BLOCK_2M_SIZE &&
                (cur_va & (ARM64_BLOCK_2M_SIZE - 1UL)) == 0UL) {
                uintptr_t pa = (uintptr_t)(l2[idx2] & ARM64_PTE_ADDR_MASK);
                uint64_t new_blk = arm64_pte_make_leaf(pa, new_prot, true, false, non_global);
                arm64_bbm_update_pte(&l2[idx2], cur_va, new_blk);
                offset += ARM64_BLOCK_2M_SIZE;
                continue;
            }
            int rc = pt_split_2m_block(&l2[idx2], cur_va, ops);
            if (rc < 0) {
                return rc;
            }
        }

        uint64_t *l3 = (uint64_t *)ops->phys_to_pt_virt(
            ops->ctx, (uintptr_t)(l2[idx2] & ARM64_PTE_ADDR_MASK));
        uint32_t idx3 = (uint32_t)(((uint64_t)cur_va >> 12) & ARM64_PT_INDEX_MASK);
        if ((l3[idx3] & ARM64_PTE_VALID) == 0ULL) {
            return -ENOENT;
        }

        uintptr_t pa = (uintptr_t)(l3[idx3] & ARM64_PTE_ADDR_MASK);
        bool keep_cont = (l3[idx3] & ARM64_PTE_CONT) != 0ULL &&
                         ((remain >= ARM64_CONT_64K_SIZE) &&
                          (cur_va & (ARM64_CONT_64K_SIZE - 1UL)) == 0UL);
        uint64_t new_leaf = arm64_pte_make_leaf(pa, new_prot, false, keep_cont, non_global);
        arm64_bbm_update_pte(&l3[idx3], cur_va, new_leaf);
        offset += PAGE_SIZE;
    }

    return 0;
}

int arm64_pt_translate(uintptr_t root_phys,
                       int va_bits,
                       uintptr_t vaddr,
                       uintptr_t *out_paddr,
                       uint64_t *out_pte,
                       const struct arm64_pt_backend *be)
{
    const struct arm64_pt_backend *ops = resolve_backend(be);
    uint64_t *l1;
    uint64_t *l2;
    uint32_t idx1;
    uint32_t idx2;
    uint32_t idx3;

    if (root_phys == 0U) {
        return -EINVAL;
    }

    l1 = pt_get_l1(root_phys, va_bits, vaddr, false, ops);
    if (l1 == NULL) {
        return -ENOENT;
    }

    idx1 = (uint32_t)(((uint64_t)vaddr >> 30) & ARM64_PT_INDEX_MASK);
    if ((l1[idx1] & ARM64_PTE_VALID) == 0ULL) {
        return -ENOENT;
    }
    if ((l1[idx1] & ARM64_PTE_TABLE) == 0ULL) {
        uintptr_t pa = (uintptr_t)(l1[idx1] & 0x0000FFFFC0000000ULL) |
                       (vaddr & (ARM64_BLOCK_1G_SIZE - 1UL));
        if (out_paddr != NULL) {
            *out_paddr = pa;
        }
        if (out_pte != NULL) {
            *out_pte = l1[idx1];
        }
        return 0;
    }

    l2 = (uint64_t *)ops->phys_to_pt_virt(
        ops->ctx, (uintptr_t)(l1[idx1] & ARM64_PTE_ADDR_MASK));
    idx2 = (uint32_t)(((uint64_t)vaddr >> 21) & ARM64_PT_INDEX_MASK);
    if ((l2[idx2] & ARM64_PTE_VALID) == 0ULL) {
        return -ENOENT;
    }
    if ((l2[idx2] & ARM64_PTE_TABLE) == 0ULL) {
        uintptr_t pa = (uintptr_t)(l2[idx2] & 0x0000FFFFFFE00000ULL) |
                       (vaddr & (ARM64_BLOCK_2M_SIZE - 1UL));
        if (out_paddr != NULL) {
            *out_paddr = pa;
        }
        if (out_pte != NULL) {
            *out_pte = l2[idx2];
        }
        return 0;
    }

    uint64_t *l3 = (uint64_t *)ops->phys_to_pt_virt(
        ops->ctx, (uintptr_t)(l2[idx2] & ARM64_PTE_ADDR_MASK));
    idx3 = (uint32_t)(((uint64_t)vaddr >> 12) & ARM64_PT_INDEX_MASK);
    if ((l3[idx3] & ARM64_PTE_VALID) == 0ULL) {
        return -ENOENT;
    }

    if (out_paddr != NULL) {
        *out_paddr = (uintptr_t)(l3[idx3] & ARM64_PTE_ADDR_MASK) |
                     (vaddr & (PAGE_SIZE - 1UL));
    }
    if (out_pte != NULL) {
        *out_pte = l3[idx3];
    }
    return 0;
}

static void pt_destroy_l2(uintptr_t l2_pa, const struct arm64_pt_backend *ops)
{
    uint64_t *l2 = (uint64_t *)ops->phys_to_pt_virt(ops->ctx, l2_pa);
    for (uint32_t j = 0U; j < ARM64_PT_ENTRIES; j++) {
        if ((l2[j] & ARM64_PTE_VALID) != 0ULL &&
            (l2[j] & ARM64_PTE_TABLE) != 0ULL) {
            ops->free_pt_page(ops->ctx, (uintptr_t)(l2[j] & ARM64_PTE_ADDR_MASK));
        }
    }
    ops->free_pt_page(ops->ctx, l2_pa);
}

static void pt_destroy_l1(uintptr_t l1_pa, const struct arm64_pt_backend *ops)
{
    uint64_t *l1 = (uint64_t *)ops->phys_to_pt_virt(ops->ctx, l1_pa);
    for (uint32_t i = 0U; i < ARM64_PT_ENTRIES; i++) {
        if ((l1[i] & ARM64_PTE_VALID) != 0ULL &&
            (l1[i] & ARM64_PTE_TABLE) != 0ULL) {
            pt_destroy_l2((uintptr_t)(l1[i] & ARM64_PTE_ADDR_MASK), ops);
        }
    }
    ops->free_pt_page(ops->ctx, l1_pa);
}

void arm64_pt_destroy(uintptr_t root_phys,
                      int va_bits,
                      const struct arm64_pt_backend *be)
{
    const struct arm64_pt_backend *ops = resolve_backend(be);
    if (root_phys == 0U) {
        return;
    }
    if (va_bits <= 39) {
        pt_destroy_l1(root_phys, ops);
        return;
    }

    uint64_t *l0 = (uint64_t *)ops->phys_to_pt_virt(ops->ctx, root_phys);
    for (uint32_t i = 0U; i < ARM64_PT_ENTRIES; i++) {
        if ((l0[i] & ARM64_PTE_VALID) != 0ULL &&
            (l0[i] & ARM64_PTE_TABLE) != 0ULL) {
            pt_destroy_l1((uintptr_t)(l0[i] & ARM64_PTE_ADDR_MASK), ops);
        }
    }
    ops->free_pt_page(ops->ctx, root_phys);
}

/* ---------------- 16-bit ASID 世代分配器 ---------------- */

uint16_t arm64_asid_alloc(uint64_t *out_epoch, bool *out_tlbi_needed)
{
    uint16_t asid;
    bool tlbi = false;

    if (g_asid_cursor == 0U || g_asid_cursor > ARM64_ASID_MAX) {
        g_asid_cursor = 1U;
        g_asid_epoch++;
        tlbi = true;
        arm64_tlbi_all();
    }
    asid = (uint16_t)g_asid_cursor++;

    if (out_epoch != NULL) {
        *out_epoch = g_asid_epoch;
    }
    if (out_tlbi_needed != NULL) {
        *out_tlbi_needed = tlbi;
    }
    return asid;
}

void arm64_asid_reset(void)
{
    g_asid_cursor = 1U;
    g_asid_epoch = 1ULL;
}

/* ---------------- 真机地址转换、ioremap 与 W^X 细化 ---------------- */

uintptr_t arm64_phys_to_virt(uintptr_t paddr)
{
    if (paddr >= (uintptr_t)ARM64_PHYSMAP_BASE) {
        return paddr;
    }
    return (uintptr_t)(ARM64_PHYSMAP_BASE + (uint64_t)paddr);
}

uintptr_t arm64_virt_to_phys(uintptr_t vaddr)
{
    if (vaddr >= (uintptr_t)ARM64_KERNEL_VBASE) {
        return (uintptr_t)((uint64_t)vaddr - ARM64_KERNEL_VBASE + ARM64_KERNEL_PBASE);
    }
    if (vaddr >= (uintptr_t)ARM64_PHYSMAP_BASE && vaddr < (uintptr_t)ARM64_MMIO_BASE) {
        return (uintptr_t)((uint64_t)vaddr - ARM64_PHYSMAP_BASE);
    }
    return vaddr;
}

#ifndef __HOST_TEST__
static uintptr_t g_kernel_ttbr1_root;

void *arm64_ioremap(uintptr_t paddr, size_t size)
{
    uintptr_t aligned_pa = paddr & PAGE_MASK;
    uintptr_t offset = paddr - aligned_pa;
    size_t aligned_sz = (size + offset + PAGE_SIZE - 1UL) & PAGE_MASK;
    uintptr_t va = (uintptr_t)(ARM64_MMIO_BASE + (uint64_t)aligned_pa);

    if (g_kernel_ttbr1_root != 0U) {
        uintptr_t cur_pa = 0U;
        if (arm64_pt_translate(g_kernel_ttbr1_root, CONFIG_ARM64_VA_BITS,
                               va, &cur_pa, NULL, NULL) != 0) {
            (void)arm64_pt_map_range(g_kernel_ttbr1_root,
                                     CONFIG_ARM64_VA_BITS,
                                     va,
                                     aligned_pa,
                                     aligned_sz,
                                     HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV,
                                     false,
                                     NULL);
            arm64_tlbi_va(va);
        }
    }
    return (void *)(va + offset);
}

void arm64_mmu_init_kernel_wx(void)
{
    extern char __kernel_text_start[];
    extern char __kernel_text_end[];
    extern char __kernel_rodata_start[];
    extern char __kernel_rodata_end[];
    extern char __kernel_data_start[];
    extern char __kernel_bss_end[];

    uintptr_t root_pa = default_alloc_pt_page(NULL);
    if (root_pa == 0U) {
        return;
    }

    /* 1. 映射 512MB DRAM 线性直映区 (Physmap) */
    (void)arm64_pt_map_range(root_pa,
                             CONFIG_ARM64_VA_BITS,
                             (uintptr_t)ARM64_PHYSMAP_BASE + 0x40000000UL,
                             0x40000000UL,
                             512UL * 1024UL * 1024UL,
                             HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                             false,
                             NULL);

    /* 2. 映射 QEMU virt 基础 MMIO 窗口 (GIC / PL011 / VirtIO: 0x08000000..0x0C000000) */
    (void)arm64_pt_map_range(root_pa,
                             CONFIG_ARM64_VA_BITS,
                             (uintptr_t)ARM64_MMIO_BASE + 0x08000000UL,
                             0x08000000UL,
                             64UL * 1024UL * 1024UL,
                             HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV,
                             false,
                             NULL);

    /* 3. 内核镜像按 4KB 页严格施加 W^X 权限 */
    uintptr_t text_s = (uintptr_t)__kernel_text_start & PAGE_MASK;
    uintptr_t text_e = ((uintptr_t)__kernel_text_end + PAGE_SIZE - 1UL) & PAGE_MASK;
    if (text_e > text_s) {
        (void)arm64_pt_map_range(root_pa, CONFIG_ARM64_VA_BITS,
                                 text_s, arm64_virt_to_phys(text_s),
                                 text_e - text_s,
                                 HAL_VM_PROT_READ | HAL_VM_PROT_EXEC,
                                 false, NULL);
    }

    uintptr_t ro_s = (uintptr_t)__kernel_rodata_start & PAGE_MASK;
    uintptr_t ro_e = ((uintptr_t)__kernel_rodata_end + PAGE_SIZE - 1UL) & PAGE_MASK;
    if (ro_e > ro_s) {
        (void)arm64_pt_map_range(root_pa, CONFIG_ARM64_VA_BITS,
                                 ro_s, arm64_virt_to_phys(ro_s),
                                 ro_e - ro_s,
                                 HAL_VM_PROT_READ,
                                 false, NULL);
    }

    uintptr_t rw_s = (uintptr_t)__kernel_data_start & PAGE_MASK;
    uintptr_t rw_e = ((uintptr_t)__kernel_bss_end + PAGE_SIZE - 1UL) & PAGE_MASK;
    if (rw_e > rw_s) {
        (void)arm64_pt_map_range(root_pa, CONFIG_ARM64_VA_BITS,
                                 rw_s, arm64_virt_to_phys(rw_s),
                                 rw_e - rw_s,
                                 HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                                 false, NULL);
    }

    g_kernel_ttbr1_root = root_pa;

#if defined(__aarch64__)
    /* 切换至全新 W^X TTBR1_EL1 并撤销早期 TTBR0_EL1 恒等映射 */
    __asm__ volatile(
        "dsb ishst\n\t"
        "msr ttbr1_el1, %0\n\t"
        "msr ttbr0_el1, xzr\n\t"
        "tlbi vmalle1is\n\t"
        "dsb sy\n\t"
        "isb"
        :: "r"((uint64_t)root_pa) : "memory"
    );
#endif
}

void arch_mmu_init(void)
{
    g_mm_ready = true;
}

int arch_map_page(unsigned long vaddr, unsigned long paddr,
                  unsigned long size, unsigned int flags)
{
    if (g_kernel_ttbr1_root == 0U) {
        return -EINVAL;
    }
    int rc = arm64_pt_map_range(g_kernel_ttbr1_root, CONFIG_ARM64_VA_BITS,
                                (uintptr_t)vaddr, (uintptr_t)paddr,
                                (size_t)size, (uint32_t)flags, false, NULL);
    if (rc == 0) {
        arm64_tlbi_va((uintptr_t)vaddr);
    }
    return rc;
}

int arch_unmap_page(unsigned long vaddr, unsigned long size)
{
    if (g_kernel_ttbr1_root == 0U) {
        return -EINVAL;
    }
    return arm64_pt_unmap_range(g_kernel_ttbr1_root, CONFIG_ARM64_VA_BITS,
                                (uintptr_t)vaddr, (size_t)size, true, NULL);
}

unsigned long arch_virt_to_phys(unsigned long vaddr)
{
    return (unsigned long)arm64_virt_to_phys((uintptr_t)vaddr);
}

unsigned long arch_phys_to_virt(unsigned long paddr)
{
    return (unsigned long)arm64_phys_to_virt((uintptr_t)paddr);
}

void arch_mmu_enable(void)
{
}

void arch_mmu_disable(void)
{
}

uintptr_t arm64_mmu_get_kernel_ttbr1(void)
{
    return g_kernel_ttbr1_root;
}

void arm64_clean_dcache_poc(uintptr_t va, size_t size)
{
#if defined(__aarch64__)
    const uintptr_t line = 64UL;
    uintptr_t addr = va & ~(line - 1UL);
    uintptr_t end = va + size;
    while (addr < end) {
        __asm__ volatile("dc cvac, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)va;
    (void)size;
#endif
}

void arm64_sync_icache_dcache(uintptr_t va, size_t size)
{
#if defined(__aarch64__)
    const uintptr_t line = 64UL;
    uintptr_t addr = va & ~(line - 1UL);
    uintptr_t end = va + size;
    while (addr < end) {
        __asm__ volatile("dc cvau, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    /* 使用核间广域无效 I-Cache，避免写高半核 VA 与 EL0 取指 VA 之间 VIPT 别名 */
    __asm__ volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
#else
    (void)va;
    (void)size;
#endif
}

void arm64_tlbi_asid(uint16_t asid)
{
#if defined(__aarch64__)
    uint64_t operand = ((uint64_t)asid & 0xFFFFULL) << 48;
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi aside1is, %0\n\t"
                     "dsb ish\n\t"
                     "isb"
                     :: "r"(operand) : "memory");
#else
    (void)asid;
#endif
}
#endif /* !__HOST_TEST__ */
