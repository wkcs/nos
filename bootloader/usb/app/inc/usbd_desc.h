/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: USB 标准描述符与 WinUSB 描述符头文件
 */

#ifndef BOOTLOADER_USB_APP_INC_USBD_DESC_H_
#define BOOTLOADER_USB_APP_INC_USBD_DESC_H_

#include <usbd_def.h>

#define USB_VID                         0x0EFFU
#define USB_PID                         0x0001U
#define USB_LANGID_STRING               0x0409U
#define USB_MANUFACTURER_STRING         "NOS Embedded"
#define USB_PRODUCT_STRING              "NK60-V2-BL"
/* Match the NK60 composite runtime identity so WebUSB grants survive reboot. */
#define USB_SERIALNUMBER_STRING         "320219198301"

extern USBD_DescriptorsTypeDef WINUSB_Desc;

#endif /* BOOTLOADER_USB_APP_INC_USBD_DESC_H_ */

