/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Host-only hardware API declarations.
 */
#ifndef TEST_USBD_WINUSB_H
#define TEST_USBD_WINUSB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define __ALIGN_BEGIN
#define __ALIGN_END
#define USB_DESC_TYPE_CONFIGURATION 2
#define USB_DESC_TYPE_INTERFACE 4
#define USB_DESC_TYPE_ENDPOINT 5
#define USB_DESC_TYPE_DEVICE_QUALIFIER 6
#define USB_LEN_DEV_QUALIFIER_DESC 10
#define WINUSB_EPOUT_ADDR 1
#define WINUSB_EPIN_ADDR 0x81
#define WINUSB_DATA_FS_MAX_PACKET_SIZE 64
#define LOBYTE(x) ((uint8_t)(x))
#define HIBYTE(x) ((uint8_t)((x) >> 8))
#define USBD_OK 0
#define USBD_FAIL 2
#define USBD_BUSY 1
#define USBD_EP_TYPE_BULK 2
#define USB_REQ_TYPE_MASK 0x60
#define USB_REQ_TYPE_VENDOR 0x40
#define USB_REQ_TYPE_STANDARD 0
#define USB_REQ_TYPE_CLASS 0x20
#define USB_REQ_GET_STATUS 0
#define USB_REQ_GET_INTERFACE 10
#define USB_REQ_SET_INTERFACE 11
typedef struct { struct { uint8_t is_used; } ep_in[16], ep_out[16]; } USBD_HandleTypeDef;
typedef struct { uint8_t bmRequest, bRequest; uint16_t wIndex, wLength; } USBD_SetupReqTypedef;
typedef struct {
 uint8_t (*Init)(USBD_HandleTypeDef *, uint8_t);
 uint8_t (*DeInit)(USBD_HandleTypeDef *, uint8_t);
 uint8_t (*Setup)(USBD_HandleTypeDef *, USBD_SetupReqTypedef *);
 void *EP0_TxSent, *EP0_RxReady;
 uint8_t (*DataIn)(USBD_HandleTypeDef *, uint8_t);
 uint8_t (*DataOut)(USBD_HandleTypeDef *, uint8_t);
 void *SOF, *IsoINIncomplete, *IsoOUTIncomplete;
 uint8_t *(*GetHSConfigDescriptor)(uint16_t *);
 uint8_t *(*GetFSConfigDescriptor)(uint16_t *);
 uint8_t *(*GetOtherSpeedConfigDescriptor)(uint16_t *);
 uint8_t *(*GetDeviceQualifierDescriptor)(uint16_t *);
} USBD_ClassTypeDef;
int USBD_LL_OpenEP(USBD_HandleTypeDef *, uint8_t, uint8_t, uint16_t);
int USBD_LL_CloseEP(USBD_HandleTypeDef *, uint8_t);
int USBD_LL_PrepareReceive(USBD_HandleTypeDef *, uint8_t, uint8_t *, uint16_t);
int USBD_LL_Transmit(USBD_HandleTypeDef *, uint8_t, uint8_t *, uint16_t);
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *, uint8_t);
uint8_t USBD_WinUSB_Transmit(const uint8_t *, uint16_t);
void USBD_WinUSB_Poll(void);
#endif
