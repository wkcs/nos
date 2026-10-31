/** Copyright (C) 2026 Nick Hu */
#include <nos/platform.h>
#include "stm32f10x.h"

/* Vendor clock and processor setup remains owned by the SoC port. */
extern void SystemInit(void);

void soc_early_init(void)
{
    SystemInit();
    DBGMCU->CR |= DBGMCU_CR_DBG_SLEEP | DBGMCU_CR_DBG_STOP | DBGMCU_CR_DBG_STANDBY;
}
