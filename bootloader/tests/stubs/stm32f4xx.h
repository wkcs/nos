/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Host-only hardware API declarations.
 */
#ifndef TEST_STM32_H
#define TEST_STM32_H
void test_system_reset(void);
#define NVIC_SystemReset test_system_reset
#include <stdint.h>
uint32_t test_get_primask(void);
void test_disable_irq(void);
void test_set_primask(uint32_t mask);
#define __get_PRIMASK test_get_primask
#define __disable_irq test_disable_irq
#define __set_PRIMASK test_set_primask
#endif
