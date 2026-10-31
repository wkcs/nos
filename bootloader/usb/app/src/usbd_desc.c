/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: USB 标准描述符与 Microsoft OS String 实现
 */

#include "usbd_desc.h"
#include <usbd_core.h>

#define USBD_VID                        USB_VID
#define USBD_PID                        USB_PID
#define USBD_LANGID_STRING              USB_LANGID_STRING
#define USBD_MANUFACTURER_STRING        USB_MANUFACTURER_STRING
#define USBD_PRODUCT_STRING             USB_PRODUCT_STRING
#define USBD_SERIALNUMBER_STRING        USB_SERIALNUMBER_STRING

static uint8_t *USBD_WinUSB_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_WinUSB_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
#if (USBD_CLASS_USER_STRING_DESC == 1)
static uint8_t *USBD_WinUSB_UsrStrDescriptor(USBD_SpeedTypeDef speed, uint8_t index, uint16_t *length);
#endif

USBD_DescriptorsTypeDef WINUSB_Desc = {
    USBD_WinUSB_DeviceDescriptor,
    USBD_WinUSB_LangIDStrDescriptor,
    USBD_WinUSB_ManufacturerStrDescriptor,
    USBD_WinUSB_ProductStrDescriptor,
    USBD_WinUSB_SerialStrDescriptor,
    USBD_WinUSB_ConfigStrDescriptor,
    USBD_WinUSB_InterfaceStrDescriptor,
#if (USBD_CLASS_USER_STRING_DESC == 1)
    USBD_WinUSB_UsrStrDescriptor,
#endif
};

/* USB 设备描述符 (VID: 0x0EFF, PID: 0x0001, Class 0xFF 厂商自定义) */
__ALIGN_BEGIN static const uint8_t USBD_FS_DeviceDesc[18] __ALIGN_END = {
    0x12,                       /* bLength */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType */
    0x00, 0x02,                 /* bcdUSB 2.00 */
    0xFF,                       /* bDeviceClass (Vendor Specific) */
    0x00,                       /* bDeviceSubClass */
    0x00,                       /* bDeviceProtocol */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize0 (64) */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID), HIBYTE(USBD_PID),
    0x00, 0x01,                 /* bcdDevice 1.00 */
    USBD_IDX_MFC_STR,           /* Index of manufacturer string */
    USBD_IDX_PRODUCT_STR,       /* Index of product string */
    USBD_IDX_SERIAL_STR,        /* Index of serial number string */
    USBD_MAX_NUM_CONFIGURATION  /* bNumConfigurations */
};

/* 语言描述符 */
__ALIGN_BEGIN static const uint8_t USBD_LangIDDesc[4] __ALIGN_END = {
    0x04,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING),
    HIBYTE(USBD_LANGID_STRING)
};

/* Microsoft OS String (Index 0xEE): "MSFT100" + Vendor Code 'A' */
__ALIGN_BEGIN static const uint8_t USBD_MS_OS_String[18] __ALIGN_END = {
    18,
    USB_DESC_TYPE_STRING,
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    'A', 0
};

__ALIGN_BEGIN static uint8_t USBD_StrDesc[USBD_MAX_STR_SPEC_SIZ] __ALIGN_END;

static uint8_t *USBD_WinUSB_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_FS_DeviceDesc);
    return (uint8_t *)USBD_FS_DeviceDesc;
}

static uint8_t *USBD_WinUSB_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_LangIDDesc);
    return (uint8_t *)USBD_LangIDDesc;
}

static uint8_t *USBD_WinUSB_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_WinUSB_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_PRODUCT_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_WinUSB_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_SERIALNUMBER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_WinUSB_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)"WinUSB Configuration", USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_WinUSB_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)"WinUSB Interface", USBD_StrDesc, length);
    return USBD_StrDesc;
}

#if (USBD_CLASS_USER_STRING_DESC == 1)
static uint8_t *USBD_WinUSB_UsrStrDescriptor(USBD_SpeedTypeDef speed, uint8_t index, uint16_t *length)
{
    (void)speed;
    if (index == 0xEEU) {
        *length = sizeof(USBD_MS_OS_String);
        return (uint8_t *)USBD_MS_OS_String;
    }
    *length = 0U;
    return NULL;
}
#endif
