/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#ifdef CONFIG_NLK_SERVER
#include <nos/nlk_core.h>
#endif

extern addr_t __mm_pool_start, __mm_pool_end;
extern addr_t __mm_sys_reserve_start, __mm_sys_reserve_end;
mm_reserve_node_register((addr_t)&__mm_sys_reserve_start, (addr_t)&__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)&__mm_pool_start, (addr_t)&__mm_pool_end, inside);

#ifdef CONFIG_NLK_SERVER
static int nutiny_nlk_setup(struct nlk_node *node)
{
    return nlk_node_set_identity(node, 0x4e4fU, 0x0472U,
                                 NLK_DEV_CLASS_GENERIC, "NuTiny-NUC472");
}
#endif

void board_early_init(void)
{
#ifdef CONFIG_NLK_SERVER
    nlk_server_register_board_setup(nutiny_nlk_setup);
#endif
}
void board_init(void)
{
    arch_irq_init();
    arch_mmu_init();
}
void board_late_init(void) { }
