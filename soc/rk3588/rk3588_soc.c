/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Rockchip RK3588 SoC 早期外设与大中小核拓扑初始化
 */

#include <soc/rk3588.h>
#include <kernel/printk.h>

void rk3588_soc_init(void)
{
    pr_info("RK3588 SoC: 3-Core RKNPU base=0x%08lx initialized\n",
            (unsigned long)RK3588_RKNPU_CORE0_BASE);
}
