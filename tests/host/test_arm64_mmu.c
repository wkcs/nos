/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Stage-1 硬件页表构建器、W^X 策略、2MB/64KB 大页与 ASID 分配器主机测试
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <asm/mmu.h>

#define FAKE_POOL_PAGES 1024U
#define FAKE_PHYS_BASE  0x40000000ULL

struct fake_mem_pool {
    uint8_t  mem[FAKE_POOL_PAGES][4096];
    bool     used[FAKE_POOL_PAGES];
    uint32_t active_pages;
};

static struct fake_mem_pool g_pool;

static uintptr_t fake_alloc_pt(void *ctx)
{
    struct fake_mem_pool *p = (struct fake_mem_pool *)ctx;
    for (uint32_t i = 0U; i < FAKE_POOL_PAGES; i++) {
        if (!p->used[i]) {
            p->used[i] = true;
            p->active_pages++;
            memset(p->mem[i], 0, 4096U);
            return (uintptr_t)(FAKE_PHYS_BASE + (uint64_t)i * 4096ULL);
        }
    }
    return 0U;
}

static void fake_free_pt(void *ctx, uintptr_t pt_phys)
{
    struct fake_mem_pool *p = (struct fake_mem_pool *)ctx;
    assert(pt_phys >= FAKE_PHYS_BASE);
    uint64_t idx = (pt_phys - FAKE_PHYS_BASE) / 4096ULL;
    assert(idx < FAKE_POOL_PAGES);
    assert(p->used[idx]);
    p->used[idx] = false;
    p->active_pages--;
}

static void *fake_pa_to_va(void *ctx, uintptr_t phys)
{
    struct fake_mem_pool *p = (struct fake_mem_pool *)ctx;
    assert(phys >= FAKE_PHYS_BASE);
    uint64_t off = phys - FAKE_PHYS_BASE;
    assert(off + 4096ULL <= sizeof(p->mem));
    return (void *)((uint8_t *)p->mem + off);
}

static const struct arm64_pt_backend g_be = {
    .alloc_pt_page   = fake_alloc_pt,
    .free_pt_page    = fake_free_pt,
    .phys_to_pt_virt = fake_pa_to_va,
    .ctx             = &g_pool,
};

static void test_pte_wx_and_flags(void)
{
    uint64_t rx_kern = arm64_pte_make_leaf(0x40200000ULL,
                                           HAL_VM_PROT_READ | HAL_VM_PROT_EXEC,
                                           false, false, false);
    assert((rx_kern & ARM64_PTE_VALID) != 0ULL);
    assert((rx_kern & ARM64_PTE_PAGE) != 0ULL);
    assert((rx_kern & ARM64_PTE_AF) != 0ULL);
    assert((rx_kern & ARM64_PTE_AP_RO) != 0ULL);
    assert((rx_kern & ARM64_PTE_UXN) != 0ULL);
    assert((rx_kern & ARM64_PTE_PXN) == 0ULL);

    uint64_t rw_user = arm64_pte_make_leaf(0x41000000ULL,
                                           HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER,
                                           false, true, true);
    assert((rw_user & ARM64_PTE_AP_EL0) != 0ULL);
    assert((rw_user & ARM64_PTE_AP_RO) == 0ULL);
    assert((rw_user & ARM64_PTE_PXN) != 0ULL);
    assert((rw_user & ARM64_PTE_UXN) != 0ULL);
    assert((rw_user & ARM64_PTE_CONT) != 0ULL);
    assert((rw_user & ARM64_PTE_NG) != 0ULL);

    uint64_t dev_pte = arm64_pte_make_leaf(0x09000000ULL,
                                           HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_DEV,
                                           true, false, false);
    assert((dev_pte & ARM64_PTE_PAGE) == 0ULL); /* 2MB Block bit1 为 0 */
    assert(((dev_pte >> 2) & 7ULL) == MT_DEVICE_nGnRnE);
}

static void test_39bit_4k_64k_2m_and_wx_reject(void)
{
    memset(&g_pool, 0, sizeof(g_pool));
    uintptr_t root = g_be.alloc_pt_page(g_be.ctx);
    assert(root != 0U);

    /* 1. W^X 强制拒绝 Write + Exec */
    int rc = arm64_pt_map_range(root, 39, 0x100000ULL, 0x42000000ULL, 4096U,
                                HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC | HAL_VM_PROT_USER,
                                true, &g_be);
    assert(rc == -EINVAL);

    /* 2. 4KB 映射与查询 */
    rc = arm64_pt_map_range(root, 39, 0x100000ULL, 0x42000000ULL, 4096U,
                            HAL_VM_PROT_READ | HAL_VM_PROT_EXEC | HAL_VM_PROT_USER,
                            true, &g_be);
    assert(rc == 0);
    uintptr_t out_pa = 0U;
    uint64_t out_pte = 0ULL;
    assert(arm64_pt_translate(root, 39, 0x100123ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x42000123ULL);
    assert((out_pte & ARM64_PTE_CONT) == 0ULL);

    /* 3. 64KB 对齐连续映射自动置位 PTE_CONT */
    rc = arm64_pt_map_range(root, 39, 0x0040000000ULL, 0x44000000ULL, ARM64_CONT_64K_SIZE,
                            HAL_VM_PROT_READ | HAL_VM_PROT_USER,
                            true, &g_be);
    assert(rc == 0);
    assert(arm64_pt_translate(root, 39, 0x0040008100ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x44008100ULL);
    assert((out_pte & ARM64_PTE_CONT) != 0ULL);

    /* 4. 2MB 对齐大页映射自动生成 L2 Block 且内部任意偏移查询准确 */
    rc = arm64_pt_map_range(root, 39, 0x0050000000ULL, 0x46000000ULL, ARM64_BLOCK_2M_SIZE,
                            HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER,
                            true, &g_be);
    assert(rc == 0);
    assert(arm64_pt_translate(root, 39, 0x0050123456ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x46123456ULL);
    assert((out_pte & ARM64_PTE_PAGE) == 0ULL); /* 确认是 Block 而不是 L3 Page */

    /* 5. 对 2MB Block 中的单个 4KB 执行 mprotect 应自动裂解为 4KB 页并精确修改权限 */
    rc = arm64_pt_mprotect(root, 39, 0x0050010000ULL, 4096U,
                           HAL_VM_PROT_READ | HAL_VM_PROT_USER, true, &g_be);
    assert(rc == 0);
    assert(arm64_pt_translate(root, 39, 0x0050010010ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x46010010ULL);
    assert((out_pte & ARM64_PTE_PAGE) != 0ULL);
    assert((out_pte & ARM64_PTE_AP_RO) != 0ULL);
    /* 相邻未修改页仍保持可写 */
    assert(arm64_pt_translate(root, 39, 0x0050011010ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x46011010ULL);
    assert((out_pte & ARM64_PTE_AP_RO) == 0ULL);

    /* 6. Unmap 并销毁后池页表必须零泄漏 */
    assert(arm64_pt_unmap_range(root, 39, 0x100000ULL, 4096U, false, &g_be) == 0);
    assert(arm64_pt_translate(root, 39, 0x100000ULL, &out_pa, &out_pte, &g_be) == -ENOENT);

    /* 7. arm64_pt_map_range_noflush 与 arm64_pt_test_and_clear_af 校验 */
    assert(arm64_pt_map_range_noflush(root, 39, 0x180000ULL, 0x48000000ULL, 4096U,
                                      HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER,
                                      true, &g_be) == 0);
    bool was_accessed = false;
    assert(arm64_pt_test_and_clear_af(root, 39, 0x180000ULL, &was_accessed, &g_be) == 0);
    assert(was_accessed);
    assert(arm64_pt_translate(root, 39, 0x180000ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == 0x48000000ULL && (out_pte & ARM64_PTE_AF) == 0ULL);
    was_accessed = true;
    assert(arm64_pt_test_and_clear_af(root, 39, 0x180000ULL, &was_accessed, &g_be) == 0);
    assert(!was_accessed);

    arm64_pt_destroy(root, 39, &g_be);
    assert(g_pool.active_pages == 0U);
}

static void test_48bit_higher_half(void)
{
    memset(&g_pool, 0, sizeof(g_pool));
    uintptr_t root = g_be.alloc_pt_page(g_be.ctx);
    assert(root != 0U);

    uintptr_t va = 0xFFFFFFF000000000ULL;
    uintptr_t pa = 0x40200000ULL;
    assert(arm64_pt_map_range(root, 48, va, pa, 8192U,
                              HAL_VM_PROT_READ | HAL_VM_PROT_EXEC, false, &g_be) == 0);
    uintptr_t out_pa = 0U;
    uint64_t out_pte = 0ULL;
    assert(arm64_pt_translate(root, 48, va + 0x1080ULL, &out_pa, &out_pte, &g_be) == 0);
    assert(out_pa == pa + 0x1080ULL);
    assert((out_pte & ARM64_PTE_UXN) != 0ULL);
    assert((out_pte & ARM64_PTE_PXN) == 0ULL);

    arm64_pt_destroy(root, 48, &g_be);
    assert(g_pool.active_pages == 0U);
}

static void test_asid_generation_wrap(void)
{
    arm64_asid_reset();
    uint64_t epoch = 0ULL;
    bool tlbi = false;
    uint16_t first = arm64_asid_alloc(&epoch, &tlbi);
    assert(first == 1U);
    assert(epoch == 1ULL);
    assert(!tlbi);

    for (uint32_t i = 2U; i <= 65535U; i++) {
        uint16_t a = arm64_asid_alloc(&epoch, &tlbi);
        assert(a == (uint16_t)i);
        assert(epoch == 1ULL);
        assert(!tlbi);
    }

    uint16_t wrapped = arm64_asid_alloc(&epoch, &tlbi);
    assert(wrapped == 1U);
    assert(epoch == 2ULL);
    assert(tlbi);
}

int main(void)
{
    test_pte_wx_and_flags();
    test_39bit_4k_64k_2m_and_wx_reject();
    test_48bit_higher_half();
    test_asid_generation_wrap();
    printf("arm64 mmu checks passed\n");
    return 0;
}
