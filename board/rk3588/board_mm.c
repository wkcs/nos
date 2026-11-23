/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Rockchip RK3588 板级内存节点注册（Higher-Half 线性直映区间）
 */

#include <kernel/kernel.h>
#include <kernel/mm.h>

extern char __mm_pool_start[];
extern char __mm_pool_end[];
extern char __mm_sys_reserve_start[];
extern char __mm_sys_reserve_end[];

mm_reserve_node_register((addr_t)__mm_sys_reserve_start, (addr_t)__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)__mm_pool_start, (addr_t)__mm_pool_end, dram0);

void board_mm_init(void)
{
}
