/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 独立 Bootloader 主入口
 * 架构规范：
 * - 采用 ST 官方 USB Device 协议栈 (STM32_USB_Device_Library + HAL PCD)
 * - 模块化拆分：drivers/, usb/, nlk/
 * - 固件严格驻留于 STM32F401 内部 Flash Sector 0 (0x08000000 - 0x08003FFF, 16KB)
 */

#include "drivers/clock.h"
#include "drivers/uart.h"
#include "drivers/rgb.h"
#include "drivers/key.h"
#include "drivers/flash.h"
#include "nlk/nlk_bl.h"
#include "usb/app/inc/usb_bsp.h"
#include <boot_proto.h>

int main(void)
{
    /* 1. 初始化 84MHz 主频时钟与 RTC 备份域写权限 */
    clock_init_84mhz();

    /* 2. 初始化串口 1 (115200 8N1) */
    uart1_init();
    uart_puts("\r\n========================================\r\n");
    uart_puts("[BL] NK60-V2 Bootloader v1.0.0 Booting...\r\n");

    /* 3. 检查启动标志与按键状态 */
    uint32_t bkp_val = backup_read_flag();
    bool stay_in_bl = false;

    if (bkp_val == (BOOT_FLAG_MAGIC | BOOT_BOOTLOADER)) {
        uart_puts("[BL] Stay in BL: Explicit RPC reboot flag detected\r\n");
        stay_in_bl = true;
    } else if (key_scan_recovery_pressed()) {
        uart_puts("[BL] Stay in BL: Emergency recovery keys detected\r\n");
        stay_in_bl = true;
    } else if (!flash_is_kernel_valid()) {
        uart_puts("[BL] Stay in BL: Kernel vector table invalid\r\n");
        stay_in_bl = true;
    }

    /* 4. 如果无需停留在 Bootloader，直接跳转引导内核 */
    if (!stay_in_bl) {
        uart_puts("[BL] Kernel valid, booting application...\r\n");
        flash_jump_to_app(NK60_KERNEL_FLASH_BASE);
    }

    /* 5. 停留在 Bootloader: 清除启动标志并初始化各服务 */
    backup_clear_flag();

    rgb_init();
    rgb_set_solid(255U, 120U, 0U); /* 橙黄指示灯常亮 */

    nlk_bl_init();

    /* 启动 ST 官方 USB Device 协议栈 */
    usb_bsp_init();
    uart_puts("[BL] Official ST USB Stack initialized (WinUSB 0x0EFF:0x0001)\r\n");
    uart_puts("[BL] Ready for NLK RPC & Flash commands.\r\n");

    /* 6. 主服务循环 */
    while (1) {
        usb_bsp_poll();
        __asm__ volatile ("wfi");
    }

    return 0;
}
