/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Host-only descriptor API declarations.
 */
#ifndef TEST_USBD_DEF_H
#define TEST_USBD_DEF_H
#include "usbd_winusb.h"
#define USB_DESC_TYPE_DEVICE 1
#define USB_DESC_TYPE_STRING 3
#define USB_MAX_EP0_SIZE 64
#define USBD_MAX_NUM_CONFIGURATION 1
#define USBD_MAX_STR_SPEC_SIZ 256
#define USBD_IDX_MFC_STR 1
#define USBD_IDX_PRODUCT_STR 2
#define USBD_IDX_SERIAL_STR 3
#define USBD_CLASS_USER_STRING_DESC 1
typedef enum { USBD_SPEED_HIGH, USBD_SPEED_FULL } USBD_SpeedTypeDef;
typedef struct {
    uint8_t *(*GetDeviceDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetLangIDStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetManufacturerStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetProductStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetSerialStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetConfigurationStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetInterfaceStrDescriptor)(USBD_SpeedTypeDef, uint16_t *);
    uint8_t *(*GetUserStrDescriptor)(USBD_SpeedTypeDef, uint8_t, uint16_t *);
} USBD_DescriptorsTypeDef;
#endif
