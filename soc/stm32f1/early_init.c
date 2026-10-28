/** Copyright (C) 2026 Nick Hu */
#include <nos/platform.h>

/* Vendor clock and processor setup remains owned by the SoC port. */
extern void SystemInit(void);

void soc_early_init(void)
{
    SystemInit();
}
