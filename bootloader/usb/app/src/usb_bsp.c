/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: USB 板级支持与生命周期管理实现
 */

#include "usb_bsp.h"
#include "usbd_desc.h"
#include "usbd_winusb.h"
#include <usbd_core.h>
#include <stm32f4xx_ll_usb.h>
#include "clock.h"
#include "uart.h"

static USBD_HandleTypeDef g_usb_device;

void usb_bsp_init(void)
{
    /* 1. 先将 PA11(DM) 与 PA12(DP) 配置为通用推挽输出并拉低 80ms，
     * 确保 Host/Hub 硬件确凿检测到总线断开 (Debounce Disconnect)，触发重新枚举 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 22U) | (3UL << 24U))) |
                   (1UL << 22U) | (1UL << 24U);
    GPIOA->BSRR = (1UL << (11U + 16U)) | (1UL << (12U + 16U));
    delay_ms(80U);

    /* 2. 初始化 USB Device 核心，绑定 WinUSB 描述符与类驱动 */
    USBD_Init(&g_usb_device, &WINUSB_Desc, 0);
    USBD_RegisterClass(&g_usb_device, &USBD_WINUSB_ClassDriver);

    /* 3. 启动 USB 协议栈与硬件端点 */
    USBD_Start(&g_usb_device);
    uart_puts("[BL_USB] USB Device Started, awaiting Host enumeration...\r\n");
}

void usb_bsp_deinit(void)
{
    USB_DevDisconnect(USB_OTG_FS);
    USBD_Stop(&g_usb_device);
    USBD_DeInit(&g_usb_device);
}

void usb_bsp_poll(void)
{
    USBD_WinUSB_Poll();
}
