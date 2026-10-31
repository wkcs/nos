/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: ST 官方 USB Device 库底层配置
 */

#ifndef BOOTLOADER_USB_APP_INC_USBD_CONF_H_
#define BOOTLOADER_USB_APP_INC_USBD_CONF_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stm32f4xx.h>
#include <stm32f4xx_hal_def.h>

#define USBD_MAX_NUM_INTERFACES             1U
#define USBD_MAX_NUM_CONFIGURATION          1U
#define USBD_MAX_STR_SPEC_SIZ               64U
#define USBD_CLASS_USER_STRING_DESC         1U
#define USBD_SELF_POWERED                   1U
#define USBD_DEBUG_LEVEL                    0U

/* 设备端点分配 */
#define WINUSB_EPIN_ADDR                    0x81U
#define WINUSB_EPOUT_ADDR                   0x01U
#define WINUSB_DATA_FS_MAX_PACKET_SIZE      64U

/* 内存与字符串宏 */
#define USBD_malloc(sz)                     ((void)0)
#define USBD_free(p)                        ((void)0)
#define USBD_memset                         memset
#define USBD_memcpy                         memcpy
#define USBD_Delay                          delay_ms

void delay_ms(uint32_t ms);

#endif /* BOOTLOADER_USB_APP_INC_USBD_CONF_H_ */
