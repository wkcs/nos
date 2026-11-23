/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Higher-Half 虚拟内存布局与 ARMv8-A Stage-1 硬件页表契约
 * 默认 39-bit 3 级页表（512GB 用户态 + 512GB 内核高半核），支持扩展至 48-bit 4 级。
 */

#ifndef __ASM_ARM64_MMU_H__
#define __ASM_ARM64_MMU_H__

#ifndef _AC
#ifdef __ASSEMBLY__
#define _AC(X, Y)   X
#define _AT(T, X)   X
#else
#define __AC(X, Y)  (X##Y)
#define _AC(X, Y)   __AC(X, Y)
#define _AT(T, X)   ((T)(X))
#endif
#endif

#ifndef CONFIG_ARM64_VA_BITS
#define CONFIG_ARM64_VA_BITS    39
#endif

#ifndef CONFIG_ARM64_PA_BITS
#define CONFIG_ARM64_PA_BITS    40
#endif

#define PAGE_SHIFT              12
#define PAGE_SIZE               (_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK               (~(PAGE_SIZE - _AC(1, UL)))

#define ARM64_BLOCK_2M_SHIFT    21
#define ARM64_BLOCK_2M_SIZE     (_AC(1, UL) << ARM64_BLOCK_2M_SHIFT)
#define ARM64_BLOCK_2M_MASK     (~(ARM64_BLOCK_2M_SIZE - _AC(1, UL)))

#define ARM64_CONT_64K_SHIFT    16
#define ARM64_CONT_64K_SIZE     (_AC(1, UL) << ARM64_CONT_64K_SHIFT)
#define ARM64_CONT_64K_PAGES    16

#define ARM64_BLOCK_1G_SHIFT    30
#define ARM64_BLOCK_1G_SIZE     (_AC(1, UL) << ARM64_BLOCK_1G_SHIFT)

/* Higher-Half 内核虚拟地址空间切分 */
#if (CONFIG_ARM64_VA_BITS == 48)
#define ARM64_HW_PGTABLE_LEVELS 4
#define ARM64_USER_VA_LIMIT     _AC(0x0001000000000000, ULL)
#define ARM64_PHYSMAP_BASE      _AC(0xFFFF800000000000, ULL)
#define ARM64_MMIO_BASE         _AC(0xFFFFC00000000000, ULL)
#define ARM64_KVMALLOC_BASE     _AC(0xFFFFD00000000000, ULL)
#define ARM64_KERNEL_VBASE      _AC(0xFFFFFFF000000000, ULL)
#define ARM64_TCR_T0SZ          (_AC(64, ULL) - _AC(48, ULL))
#define ARM64_TCR_T1SZ          (_AC(64, ULL) - _AC(48, ULL))
#else
#define ARM64_HW_PGTABLE_LEVELS 3
#define ARM64_USER_VA_LIMIT     _AC(0x0000008000000000, ULL)
#define ARM64_PHYSMAP_BASE      _AC(0xFFFFFF8000000000, ULL)
#define ARM64_MMIO_BASE         _AC(0xFFFFFFC000000000, ULL)
#define ARM64_KVMALLOC_BASE     _AC(0xFFFFFFD000000000, ULL)
#define ARM64_KERNEL_VBASE      _AC(0xFFFFFFF000000000, ULL)
#define ARM64_TCR_T0SZ          (_AC(64, ULL) - _AC(39, ULL))
#define ARM64_TCR_T1SZ          (_AC(64, ULL) - _AC(39, ULL))
#endif

/* 早期物理引导布局（支持 QEMU virt @ 0x40200000 与 RK3588 @ 0x00200000） */
#ifdef CONFIG_SOC_RK3588
#define ARM64_DRAM_BASE             _AC(0x00000000, ULL)
#define ARM64_DRAM_L1_IDX           0
#define ARM64_EARLY_MMIO_BASE       _AC(0xFC000000, ULL)
#define ARM64_EARLY_MMIO_SIZE       _AC(0x04000000, ULL)
#define ARM64_EARLY_MMIO_L1_IDX     3
#define ARM64_EARLY_MMIO_L2_START   480
#define ARM64_EARLY_MMIO_L2_END     512
#ifndef ARM64_KERNEL_PBASE
#define ARM64_KERNEL_PBASE          _AC(0x00200000, ULL)
#endif
#else
#define ARM64_DRAM_BASE             _AC(0x40000000, ULL)
#define ARM64_DRAM_L1_IDX           1
#define ARM64_EARLY_MMIO_BASE       _AC(0x08000000, ULL)
#define ARM64_EARLY_MMIO_SIZE       _AC(0x04000000, ULL)
#define ARM64_EARLY_MMIO_L1_IDX     0
#define ARM64_EARLY_MMIO_L2_START   64
#define ARM64_EARLY_MMIO_L2_END     96
#ifndef ARM64_KERNEL_PBASE
#define ARM64_KERNEL_PBASE          _AC(0x40200000, ULL)
#endif
#endif

/* MAIR_EL1 内存属性索引定义 */
#define MT_DEVICE_nGnRnE        0
#define MT_DEVICE_nGnRE         1
#define MT_NORMAL_NC            2
#define MT_NORMAL_WT            3
#define MT_NORMAL               4
#define MT_NORMAL_TAGGED        5

#define ARM64_MAIR_VALUE        ( \
    (_AC(0x00, ULL) << (MT_DEVICE_nGnRnE * 8)) | \
    (_AC(0x04, ULL) << (MT_DEVICE_nGnRE  * 8)) | \
    (_AC(0x44, ULL) << (MT_NORMAL_NC     * 8)) | \
    (_AC(0xBB, ULL) << (MT_NORMAL_WT     * 8)) | \
    (_AC(0xFF, ULL) << (MT_NORMAL        * 8)) | \
    (_AC(0xF0, ULL) << (MT_NORMAL_TAGGED * 8))   \
)

/* TCR_EL1 标准配置组合位（动态补充 IPS 字段） */
#define ARM64_TCR_TG0_4K        (_AC(0, ULL) << 14)
#define ARM64_TCR_TG1_4K        (_AC(2, ULL) << 30)
#define ARM64_TCR_SH0_INNER     (_AC(3, ULL) << 12)
#define ARM64_TCR_SH1_INNER     (_AC(3, ULL) << 28)
#define ARM64_TCR_ORGN0_WBWA    (_AC(1, ULL) << 10)
#define ARM64_TCR_IRGN0_WBWA    (_AC(1, ULL) << 8)
#define ARM64_TCR_ORGN1_WBWA    (_AC(1, ULL) << 26)
#define ARM64_TCR_IRGN1_WBWA    (_AC(1, ULL) << 24)
#define ARM64_TCR_AS_16BIT      (_AC(1, ULL) << 36)

#define ARM64_TCR_BASE ( \
    (ARM64_TCR_T0SZ << 0)  | (ARM64_TCR_T1SZ << 16) | \
    ARM64_TCR_TG0_4K       | ARM64_TCR_TG1_4K       | \
    ARM64_TCR_SH0_INNER    | ARM64_TCR_SH1_INNER    | \
    ARM64_TCR_ORGN0_WBWA   | ARM64_TCR_IRGN0_WBWA   | \
    ARM64_TCR_ORGN1_WBWA   | ARM64_TCR_IRGN1_WBWA   | \
    ARM64_TCR_AS_16BIT                                \
)

/* ARMv8-A Stage-1 硬件页表描述符标准位 */
#define ARM64_PTE_VALID         (_AC(1, ULL) << 0)
#define ARM64_PTE_TABLE         (_AC(1, ULL) << 1)
#define ARM64_PTE_PAGE          (_AC(1, ULL) << 1)
#define ARM64_PTE_BLOCK         (_AC(0, ULL) << 1)
#define ARM64_PTE_ATTRINDX(m)   ((_AT(uint64_t, m) & _AC(7, ULL)) << 2)
#define ARM64_PTE_NS            (_AC(1, ULL) << 5)
#define ARM64_PTE_AP_EL0        (_AC(1, ULL) << 6)      /* AP[1] = 1: EL0 可访问 */
#define ARM64_PTE_AP_RO         (_AC(1, ULL) << 7)      /* AP[2] = 1: 只读 */
#define ARM64_PTE_SH_NONE       (_AC(0, ULL) << 8)
#define ARM64_PTE_SH_OUTER      (_AC(2, ULL) << 8)
#define ARM64_PTE_SH_INNER      (_AC(3, ULL) << 8)
#define ARM64_PTE_AF            (_AC(1, ULL) << 10)     /* Access Flag */
#define ARM64_PTE_NG            (_AC(1, ULL) << 11)     /* Non-Global */
#define ARM64_PTE_GP            (_AC(1, ULL) << 50)     /* Guarded Page (BTI 防 JOP) */
#define ARM64_PTE_CONT          (_AC(1, ULL) << 52)     /* 64KB 连续 TLB 合并提示位 */
#define ARM64_PTE_PXN           (_AC(1, ULL) << 53)     /* 特权级不可执行 */
#define ARM64_PTE_UXN           (_AC(1, ULL) << 54)     /* 用户态不可执行 */
#define ARM64_PTE_SW_COW        (_AC(1, ULL) << 55)     /* 软件位：写时复制标记 */
#define ARM64_PTE_SW_OWNED      (_AC(1, ULL) << 56)     /* 软件位：由空间持有并负责释放 */
#define ARM64_PTE_ADDR_MASK     _AC(0x0000FFFFFFFFF000, ULL)

#ifndef __ASSEMBLY__
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct nos_vm_space;

/* 可插拔页表分配与物理地址转换后端（支持真机直映与主机单测隔离） */
struct arm64_pt_backend {
    uintptr_t (*alloc_pt_page)(void *ctx);
    void      (*free_pt_page)(void *ctx, uintptr_t pt_phys);
    void     *(*phys_to_pt_virt)(void *ctx, uintptr_t phys);
    void     *ctx;
};

/* 核心页表描述符构建与操作接口 */
uint64_t arm64_pte_make_leaf(uintptr_t paddr,
                             uint32_t prot_flags,
                             bool is_block,
                             bool contiguous,
                             bool non_global);

int arm64_pt_map_range(uintptr_t root_phys,
                       int va_bits,
                       uintptr_t vaddr,
                       uintptr_t paddr,
                       size_t size,
                       uint32_t prot_flags,
                       bool non_global,
                       const struct arm64_pt_backend *be);

int arm64_pt_unmap_range(uintptr_t root_phys,
                         int va_bits,
                         uintptr_t vaddr,
                         size_t size,
                         bool flush_tlb,
                         const struct arm64_pt_backend *be);

int arm64_pt_mprotect(uintptr_t root_phys,
                      int va_bits,
                      uintptr_t vaddr,
                      size_t size,
                      uint32_t new_prot,
                      bool non_global,
                      const struct arm64_pt_backend *be);

int arm64_pt_translate(uintptr_t root_phys,
                       int va_bits,
                       uintptr_t vaddr,
                       uintptr_t *out_paddr,
                       uint64_t *out_pte,
                       const struct arm64_pt_backend *be);

void arm64_pt_destroy(uintptr_t root_phys,
                      int va_bits,
                      const struct arm64_pt_backend *be);

/* 16-bit ASID 世代分配器（返回非 0 ASID，回绕时自动递增 epoch 并广播 TLBI） */
uint16_t arm64_asid_alloc(uint64_t *out_epoch, bool *out_tlbi_needed);
void arm64_asid_reset(void);

/* 物理内存线性直映与 MMIO ioremap 接口 */
uintptr_t arm64_phys_to_virt(uintptr_t paddr);
uintptr_t arm64_virt_to_phys(uintptr_t vaddr);
void *arm64_ioremap(uintptr_t paddr, size_t size);
void arm64_mmu_init_kernel_wx(void);
uintptr_t arm64_mmu_get_kernel_ttbr1(void);
void arm64_clean_dcache_poc(uintptr_t va, size_t size);
void arm64_sync_icache_dcache(uintptr_t va, size_t size);
void arm64_tlbi_asid(uint16_t asid);

/* 兼容旧 HAL 声明的架构 MMU 接口 */
void arch_mmu_init(void);
int arch_map_page(unsigned long vaddr, unsigned long paddr,
                  unsigned long size, unsigned int flags);
int arch_unmap_page(unsigned long vaddr, unsigned long size);
unsigned long arch_virt_to_phys(unsigned long vaddr);
unsigned long arch_phys_to_virt(unsigned long paddr);
void arch_mmu_enable(void);
void arch_mmu_disable(void);

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_ARM64_MMU_H__ */
