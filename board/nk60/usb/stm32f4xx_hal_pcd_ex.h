/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32F4XX_HAL_PCD_EX_H
#define __STM32F4XX_HAL_PCD_EX_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal_def.h"

HAL_StatusTypeDef HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *hpcd, uint8_t fifo, uint16_t size);
HAL_StatusTypeDef HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *hpcd, uint16_t size);

#endif