/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_ARM_MMU_H__
#define __ASM_ARM_MMU_H__

#include <kernel/types.h>

/* ARM 架构 MMU 管理接口 */

/* 页面大小定义 */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* 页面属性标志 */
#define PAGE_PRESENT    0x01
#define PAGE_WRITABLE   0x02
#define PAGE_USER       0x04
#define PAGE_NOCACHE    0x08

/**
 * 初始化 MMU
 */
void arch_mmu_init(void);

/**
 * 映射虚拟地址到物理地址
 * @vaddr: 虚拟地址
 * @paddr: 物理地址
 * @size: 映射大小
 * @flags: 页面属性标志
 * @return: 0 成功，负数表示错误
 */
int arch_map_page(unsigned long vaddr, unsigned long paddr, 
                  unsigned long size, unsigned int flags);

/**
 * 取消虚拟地址映射
 * @vaddr: 虚拟地址
 * @size: 取消映射的大小
 * @return: 0 成功，负数表示错误
 */
int arch_unmap_page(unsigned long vaddr, unsigned long size);

/**
 * 虚拟地址到物理地址转换
 * @vaddr: 虚拟地址
 * @return: 物理地址，0 表示转换失败
 */
unsigned long arch_virt_to_phys(unsigned long vaddr);

/**
 * 物理地址到虚拟地址转换
 * @paddr: 物理地址
 * @return: 虚拟地址
 */
unsigned long arch_phys_to_virt(unsigned long paddr);

/**
 * 启用 MMU
 */
void arch_mmu_enable(void);

/**
 * 禁用 MMU
 */
void arch_mmu_disable(void);

#endif /* __ASM_ARM_MMU_H__ */