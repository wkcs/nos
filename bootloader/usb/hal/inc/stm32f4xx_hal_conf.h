/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: STM32F4 HAL 精简模块配置
 */

#ifndef BOOTLOADER_USB_HAL_INC_STM32F4XX_HAL_CONF_H_
#define BOOTLOADER_USB_HAL_INC_STM32F4XX_HAL_CONF_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MODULE_ENABLED
#define HAL_PCD_MODULE_ENABLED

/* 振荡器频率定义 */
#if !defined  (HSE_VALUE)
  #define HSE_VALUE    8000000U /* 8 MHz 外部晶振 */
#endif

#if !defined  (HSE_STARTUP_TIMEOUT)
  #define HSE_STARTUP_TIMEOUT    100U
#endif

#if !defined  (HSI_VALUE)
  #define HSI_VALUE    16000000U /* 16 MHz 内部 RC */
#endif

#if !defined  (LSI_VALUE)
  #define LSI_VALUE    32000U
#endif

#if !defined  (LSE_VALUE)
  #define LSE_VALUE    32768U
#endif

#if !defined  (LSE_STARTUP_TIMEOUT)
  #define LSE_STARTUP_TIMEOUT    5000U
#endif

#if !defined  (EXTERNAL_CLOCK_VALUE)
  #define EXTERNAL_CLOCK_VALUE    12288000U
#endif

#define VDD_VALUE               3300U
#define TICK_INT_PRIORITY       0U
#define USE_RTOS                0U
#define PREFETCH_ENABLE         1U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE       1U

#define USE_HAL_PCD_REGISTER_CALLBACKS 0U

#define assert_param(expr) ((void)0U)

uint32_t HAL_RCC_GetHCLKFreq(void);

#ifdef HAL_PCD_MODULE_ENABLED
 #include "stm32f4xx_hal_pcd.h"
 #include "stm32f4xx_hal_pcd_ex.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_USB_HAL_INC_STM32F4XX_HAL_CONF_H_ */
