/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 SoC 早期初始化入口 (对接 arch/arm/cortex_m/start.S)
 */
#include <nos/platform.h>

extern void SystemInit(void);

void soc_early_init(void)
{
    SystemInit();
}
