/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NuTiny-NUC472 CON2 全速 USB 复合设备 (WinUSB + CDC + HID) 装配与板级初始化
 */
#include <kernel/kernel.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <drivers/usb/usb_device.h>
#include <soc/nuc472_soc.h>
#include <stdio.h>
#include <string.h>

extern struct udcd g_nuc472_udcd;

static char s_serial_str[25] = "000000000000000000000000";

static const char *s_nutiny_ustring[] = {
    "Language",
    "Nuvoton",
    "NuTiny-NUC472 NOS",
    s_serial_str,
    "Configuration",
    "Interface",
    USB_STRING_OS
};

static struct udevice_descriptor s_nutiny_dev_desc = {
    .bLength            = USB_DESC_LENGTH_DEVICE,
    .type               = USB_DESC_TYPE_DEVICE,
    .bcdUSB             = 0x0200, /* USB 2.0 全速 */
    .bDeviceClass       = USB_CLASS_MISC,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 0x40,
    .idVendor           = 0x0416,
    .idProduct          = 0x5011,
    .bcdDevice          = 0x0200,
    .iManufacturer      = USB_STRING_MANU_INDEX,
    .iProduct           = USB_STRING_PRODUCT_INDEX,
    .iSerialNumber      = USB_STRING_SERIAL_INDEX,
    .bNumConfigurations = USB_DYNAMIC,
};

int usb_hw_init(void)
{
    uint32_t uid[3] = {0};
    if (nuc472_fmc_read_uid(uid) == 0) {
        snprintf(s_serial_str, sizeof(s_serial_str), "%08X%08X%08X",
                 (unsigned int)uid[0], (unsigned int)uid[1], (unsigned int)uid[2]);
    }

    g_nuc472_udcd.device_is_hs = 0; /* 锁定为 Full-Speed 12 Mbps */

    /* 依次注册 USB 类：WinUSB, CDC (VCOM), HID */
    usb_class_register();

    /* 初始化 USB 设备核心 */
    int ret = usb_device_init(&g_nuc472_udcd);
    if (ret != 0) {
        pr_err("usb_device_init failed: %d\n", ret);
        return ret;
    }

    /* 覆盖板级自定义设备描述符与字符串 */
    udevice_t udevice = usbd_find_device(&g_nuc472_udcd);
    if (udevice != NULL) {
        usbd_device_set_descriptor(udevice, &s_nutiny_dev_desc);
        usbd_device_set_string(udevice, s_nutiny_ustring);
    }

    return 0;
}

int nutiny_usbd_register(void)
{
    return usb_hw_init();
}
task_init(nutiny_usbd_register);
