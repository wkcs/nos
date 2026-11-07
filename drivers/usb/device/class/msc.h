/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __USBDEVICE_CLASS_MSC_H__
#define __USBDEVICE_CLASS_MSC_H__

#include <kernel/types.h>

/*
 * 大容量存储设备 (Mass Storage) 相关定义
 */
#define USBREQ_GET_MAX_LUN              0xfe
#define USBREQ_MASS_STORAGE_RESET       0xff

#define SIZEOF_CSW                      0x0d
#define SIZEOF_CBW                      0x1f
#define SIZEOF_INQUIRY_CMD              0x24
#define SIZEOF_MODE_SENSE_6             0x4
#define SIZEOF_READ_CAPACITIES          0xc
#define SIZEOF_READ_CAPACITY            0x8
#define SIZEOF_REQUEST_SENSE            0x12

#define CBWFLAGS_DIR_M                  0x80
#define CBWFLAGS_DIR_IN                 0x80
#define CBWFLAGS_DIR_OUT                0x00

#define SCSI_TEST_UNIT_READY            0x00
#define SCSI_REQUEST_SENSE              0x03
#define SCSI_INQUIRY_CMD                0x12
#define SCSI_ALLOW_REMOVAL              0x1e
#define SCSI_MODE_SENSE_6               0x1a
#define SCSI_START_STOP                 0x1b
#define SCSI_READ_CAPACITIES            0x23
#define SCSI_READ_CAPACITY              0x25
#define SCSI_READ_10                    0x28
#define SCSI_WRITE_10                   0x2a
#define SCSI_VERIFY_10                  0x2f

#define CBW_SIGNATURE                   0x43425355
#define CSW_SIGNATURE                   0x53425355
#define CBW_TAG_VALUE                   0x12345678

#pragma pack(1)

struct ustorage_cbw
{
    uint32_t signature;
    uint32_t tag;
    uint32_t xfer_len;
    uint8_t dflags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
};
typedef struct ustorage_cbw* ustorage_cbw_t;

struct ustorage_csw
{
    uint32_t signature;
    uint32_t tag;
    int32_t data_reside;
    uint8_t  status;
};
typedef struct ustorage_csw* ustorage_csw_t;

#pragma pack()

#endif /* __USBDEVICE_CLASS_MSC_H__ */
