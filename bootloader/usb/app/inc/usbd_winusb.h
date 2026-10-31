/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: WinUSB Bulk 类驱动头文件
 */

#ifndef BOOTLOADER_USB_APP_INC_USBD_WINUSB_H_
#define BOOTLOADER_USB_APP_INC_USBD_WINUSB_H_

#include <usbd_ioreq.h>
#include "usbd_conf.h"

extern USBD_ClassTypeDef USBD_WINUSB_ClassDriver;

/**
 * 通过 Bulk IN 端点向 Host 发送数据 (支持多包与分块)
 */
uint8_t USBD_WinUSB_Transmit(const uint8_t *buf, uint16_t len);

/**
 * 轮询处理 USB 状态与后台软重启事件
 */
void USBD_WinUSB_Poll(void);

#endif /* BOOTLOADER_USB_APP_INC_USBD_WINUSB_H_ */

