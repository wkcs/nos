/**
  ******************************************************************************
  * @file    qemu_sd.h
  * @brief   This file contains all the functions prototypes for the QEMU SD
  *          driver (Semihosting).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __QEMU_SD_H
#define __QEMU_SD_H

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdint.h>
     
// Use uint8_t/uint32_t to avoid conflicts with diskio.h/integer.h

typedef struct
{
  uint64_t CardCapacity;  /*!< Card Capacity */
  uint32_t CardBlockSize; /*!< Card Block Size */
} QEMU_SD_CardInfo;

extern QEMU_SD_CardInfo SDCardInfo;

uint8_t QEMU_SD_Init (uint8_t pdrv);
uint8_t QEMU_SD_Status (uint8_t pdrv);
uint8_t QEMU_SD_Read (uint8_t pdrv, uint8_t *buff, uint32_t sector, uint32_t count);
uint8_t QEMU_SD_Write (uint8_t pdrv, const uint8_t *buff, uint32_t sector, uint32_t count);
uint8_t QEMU_SD_Ioctl (uint8_t pdrv, uint8_t cmd, void *buff);

#ifdef __cplusplus
}
#endif

#endif /* __QEMU_SD_H */
