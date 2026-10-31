/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: USB 板级支持与生命周期管理头文件
 */

#ifndef BOOTLOADER_USB_APP_INC_USB_BSP_H_
#define BOOTLOADER_USB_APP_INC_USB_BSP_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * 初始化并启动基于 ST 官方协议栈的 USB WinUSB 服务
 */
void usb_bsp_init(void);

/**
 * 停止并释放 USB 外设 (在跳转应用前调用)
 */
void usb_bsp_deinit(void);

/**
 * 主循环轮询处理
 */
void usb_bsp_poll(void);

#endif /* BOOTLOADER_USB_APP_INC_USB_BSP_H_ */

