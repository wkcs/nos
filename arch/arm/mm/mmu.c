/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <asm/mmu.h>
#include <kernel/printk.h>

/* ARM Cortex-M 系列通常没有 MMU，提供简化的实现 */

/**
 * 初始化 MMU (Cortex-M 系列为空实现)
 */
void arch_mmu_init(void)
{
    pr_info("ARM Cortex-M: No MMU support\n");
}

/**
 * 映射虚拟地址到物理地址 (Cortex-M 系列为恒等映射)
 * @vaddr: 虚拟地址
 * @paddr: 物理地址
 * @size: 映射大小
 * @flags: 页面属性标志
 * @return: 0 成功，负数表示错误
 */
int arch_map_page(unsigned long vaddr, unsigned long paddr, 
                  unsigned long size, unsigned int flags)
{
    /* Cortex-M 系列使用恒等映射 */
    if (vaddr != paddr) {
        pr_err("ARM Cortex-M: Virtual address must equal physical address\n");
        return -1;
    }
    
    return 0;
}

/**
 * 取消虚拟地址映射 (Cortex-M 系列为空实现)
 * @vaddr: 虚拟地址
 * @size: 取消映射的大小
 * @return: 0 成功，负数表示错误
 */
int arch_unmap_page(unsigned long vaddr, unsigned long size)
{
    /* Cortex-M 系列无需取消映射 */
    return 0;
}

/**
 * 虚拟地址到物理地址转换 (Cortex-M 系列为恒等映射)
 * @vaddr: 虚拟地址
 * @return: 物理地址
 */
unsigned long arch_virt_to_phys(unsigned long vaddr)
{
    return vaddr;
}

/**
 * 物理地址到虚拟地址转换 (Cortex-M 系列为恒等映射)
 * @paddr: 物理地址
 * @return: 虚拟地址
 */
unsigned long arch_phys_to_virt(unsigned long paddr)
{
    return paddr;
}

/**
 * 启用 MMU (Cortex-M 系列为空实现)
 */
void arch_mmu_enable(void)
{
    /* Cortex-M 系列无 MMU */
}

/**
 * 禁用 MMU (Cortex-M 系列为空实现)
 */
void arch_mmu_disable(void)
{
    /* Cortex-M 系列无 MMU */
}