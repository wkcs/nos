/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __ASM_X86_MMU_H__
#define __ASM_X86_MMU_H__

#include <kernel/types.h>

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1UL << PAGE_SHIFT)
#define PAGE_MASK   (~(PAGE_SIZE - 1))

void arch_mmu_init(void);
int arch_map_page(unsigned long vaddr, unsigned long paddr,
                  unsigned long size, unsigned int flags);
int arch_unmap_page(unsigned long vaddr, unsigned long size);
unsigned long arch_virt_to_phys(unsigned long vaddr);
unsigned long arch_phys_to_virt(unsigned long paddr);
void arch_mmu_enable(void);
void arch_mmu_disable(void);

#endif /* __ASM_X86_MMU_H__ */
