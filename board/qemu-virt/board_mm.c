/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt 内存节点注册（使用 Higher-Half 线性直映区间地址）
 */

#include <kernel/kernel.h>
#include <kernel/mm.h>

#ifdef CONFIG_ARCH_ARM64
extern char __mm_pool_start[];
extern char __mm_pool_end[];
extern char __mm_sys_reserve_start[];
extern char __mm_sys_reserve_end[];

mm_reserve_node_register((addr_t)__mm_sys_reserve_start, (addr_t)__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)__mm_pool_start, (addr_t)__mm_pool_end, dram0);
#endif

void board_mm_init(void)
{
}
