/* Copyright (C) 2026 Nick Hu */
#if defined(CONFIG_SOC_STM32F401)
#include "irq_f401.inc"
#elif defined(CONFIG_SOC_STM32F405)
#include "irq_f405.inc"
#else
#error "No STM32F4 interrupt vector table for the configured SoC part"
#endif
