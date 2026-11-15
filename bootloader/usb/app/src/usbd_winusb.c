/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: WinUSB Bulk 类驱动与 NLK 报文收发流水线实现
 */

#include "usbd_winusb.h"
#include <usbd_ctlreq.h>
#include <boot_proto.h>
#include <stm32f4xx.h>
#include "nlk_bl.h"
#include "clock.h"
#include "uart.h"

#define USB_WINUSB_CONFIG_DESC_SIZ          32U

static uint8_t USBD_WinUSB_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_WinUSB_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_WinUSB_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t USBD_WinUSB_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t USBD_WinUSB_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t *USBD_WinUSB_GetCfgDesc(uint16_t *length);
static uint8_t *USBD_WinUSB_GetDeviceQualifierDesc(uint16_t *length);

/* 使用指定初始化器确保函数指针在任何编译器或宏配置下严格对齐 */
USBD_ClassTypeDef USBD_WINUSB_ClassDriver = {
    .Init                         = USBD_WinUSB_Init,
    .DeInit                       = USBD_WinUSB_DeInit,
    .Setup                        = USBD_WinUSB_Setup,
    .EP0_TxSent                   = NULL,
    .EP0_RxReady                  = NULL,
    .DataIn                       = USBD_WinUSB_DataIn,
    .DataOut                      = USBD_WinUSB_DataOut,
    .SOF                          = NULL,
    .IsoINIncomplete              = NULL,
    .IsoOUTIncomplete             = NULL,
    .GetHSConfigDescriptor        = USBD_WinUSB_GetCfgDesc,
    .GetFSConfigDescriptor        = USBD_WinUSB_GetCfgDesc,
    .GetOtherSpeedConfigDescriptor= USBD_WinUSB_GetCfgDesc,
    .GetDeviceQualifierDescriptor = USBD_WinUSB_GetDeviceQualifierDesc,
};

/* USB WinUSB 设备配置描述符 (32 字节) */
__ALIGN_BEGIN static const uint8_t USBD_WinUSB_CfgDesc[USB_WINUSB_CONFIG_DESC_SIZ] __ALIGN_END = {
    /* Configuration Descriptor (9 字节) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIGURATION,        /* bDescriptorType */
    USB_WINUSB_CONFIG_DESC_SIZ, 0x00,   /* wTotalLength (32) */
    0x01,                               /* bNumInterfaces (1) */
    0x01,                               /* bConfigurationValue (1) */
    0x00,                               /* iConfiguration (0) */
    0x80,                               /* bmAttributes (Bus Powered) */
    250,                                /* MaxPower (500mA: 250 * 2mA) */

    /* Interface Descriptor (9 字节) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    0x00,                               /* bInterfaceNumber (0) */
    0x00,                               /* bAlternateSetting (0) */
    0x02,                               /* bNumEndpoints (2) */
    0xFF,                               /* bInterfaceClass (Vendor Specific) */
    0x4E,                               /* bInterfaceSubClass ('N') */
    0x01,                               /* bInterfaceProtocol (NLK v1) */
    0x00,                               /* iInterface (0) */

    /* Endpoint 1 OUT Descriptor (7 字节) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    WINUSB_EPOUT_ADDR,                  /* bEndpointAddress (0x01) */
    0x02,                               /* bmAttributes (Bulk) */
    LOBYTE(WINUSB_DATA_FS_MAX_PACKET_SIZE),
    HIBYTE(WINUSB_DATA_FS_MAX_PACKET_SIZE),
    0x00,                               /* bInterval */

    /* Endpoint 1 IN Descriptor (7 字节) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    WINUSB_EPIN_ADDR,                   /* bEndpointAddress (0x81) */
    0x02,                               /* bmAttributes (Bulk) */
    LOBYTE(WINUSB_DATA_FS_MAX_PACKET_SIZE),
    HIBYTE(WINUSB_DATA_FS_MAX_PACKET_SIZE),
    0x00,                               /* bInterval */
};

/* Microsoft OS 1.0 Extended Compat ID 描述符 (40 字节) */
__ALIGN_BEGIN static const uint8_t USBD_WinUSB_CompatID[40] __ALIGN_END = {
    40, 0, 0, 0,                        /* dwLength */
    0x00, 0x01,                         /* bcdVersion (1.00) */
    0x04, 0x00,                         /* wIndex (0x0004) */
    1,                                  /* bCount (1 Interface) */
    0, 0, 0, 0, 0, 0, 0,                /* Reserved */
    0,                                  /* bInterfaceNumber */
    1,                                  /* Reserved */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0, /* Compatible ID: "WINUSB\0\0" */
    0, 0, 0, 0, 0, 0, 0, 0,             /* SubCompatible ID */
    0, 0, 0, 0, 0, 0                    /* Reserved */
};

/* Microsoft OS 1.0 Extended Properties 特征描述符 (10 字节空结构) */
__ALIGN_BEGIN static const uint8_t USBD_WinUSB_ExtProp[10] __ALIGN_END = {
    10, 0, 0, 0,
    0x00, 0x01,
    0x05, 0x00,
    0, 0
};

/* 设备限定描述符 */
__ALIGN_BEGIN static const uint8_t USBD_WinUSB_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
    USB_LEN_DEV_QUALIFIER_DESC,
    USB_DESC_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02,
    0x00, 0x00, 0x00,
    0x40,
    0x01,
    0x00,
};

static USBD_HandleTypeDef *g_pusbd_dev = NULL;
static volatile bool g_tx_busy = false;
static volatile bool g_tx_zlp = false;
static volatile bool g_rx_ready = false;
static uint16_t g_reply_len = 0U;
static volatile uint32_t g_usb_generation;

/* 报文收发缓冲区 */
__ALIGN_BEGIN static uint8_t g_ep_rx_temp[WINUSB_DATA_FS_MAX_PACKET_SIZE] __ALIGN_END;
__ALIGN_BEGIN static uint8_t g_nlk_rx_buf[BL_NLK_MTU] __ALIGN_END;
__ALIGN_BEGIN static uint8_t g_nlk_tx_buf[BL_NLK_MTU] __ALIGN_END;
/* Keep a request immutable if USB resets and rearms OUT during Flash writes. */
__ALIGN_BEGIN static uint8_t g_nlk_req_buf[BL_NLK_MTU] __ALIGN_END;
static uint16_t g_nlk_rx_pos = 0U;

static uint8_t *USBD_WinUSB_GetCfgDesc(uint16_t *length)
{
    *length = sizeof(USBD_WinUSB_CfgDesc);
    return (uint8_t *)USBD_WinUSB_CfgDesc;
}

static uint8_t *USBD_WinUSB_GetDeviceQualifierDesc(uint16_t *length)
{
    *length = sizeof(USBD_WinUSB_DeviceQualifierDesc);
    return (uint8_t *)USBD_WinUSB_DeviceQualifierDesc;
}

static uint8_t USBD_WinUSB_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    ++g_usb_generation;
    g_pusbd_dev = pdev;

    /* 打开 Bulk IN / OUT 端点 */
    USBD_LL_OpenEP(pdev, WINUSB_EPIN_ADDR, USBD_EP_TYPE_BULK, WINUSB_DATA_FS_MAX_PACKET_SIZE);
    pdev->ep_in[WINUSB_EPIN_ADDR & 0x7FU].is_used = 1U;

    USBD_LL_OpenEP(pdev, WINUSB_EPOUT_ADDR, USBD_EP_TYPE_BULK, WINUSB_DATA_FS_MAX_PACKET_SIZE);
    pdev->ep_out[WINUSB_EPOUT_ADDR & 0x7FU].is_used = 1U;

    g_nlk_rx_pos = 0U;
    g_tx_busy = false;
    g_tx_zlp = false;
    g_rx_ready = false;
    g_reply_len = 0U;

    /* 启动首次 OUT 端点监听 */
    USBD_LL_PrepareReceive(pdev, WINUSB_EPOUT_ADDR, g_ep_rx_temp, WINUSB_DATA_FS_MAX_PACKET_SIZE);
    uart_puts("[BL_USB] Endpoints 0x01/0x81 Opened\n");
    return USBD_OK;
}

static uint8_t USBD_WinUSB_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    USBD_LL_CloseEP(pdev, WINUSB_EPIN_ADDR);
    pdev->ep_in[WINUSB_EPIN_ADDR & 0x7FU].is_used = 0U;

    USBD_LL_CloseEP(pdev, WINUSB_EPOUT_ADDR);
    pdev->ep_out[WINUSB_EPOUT_ADDR & 0x7FU].is_used = 0U;

    ++g_usb_generation;
    g_pusbd_dev = NULL;
    g_tx_busy = false;
    g_tx_zlp = false;
    g_rx_ready = false;
    g_reply_len = 0U;
    g_nlk_rx_pos = 0U;
    return USBD_OK;
}

static uint8_t USBD_WinUSB_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    uint16_t len = 0U;
    const uint8_t *pbuf = NULL;
    static const uint16_t status_info = 0x0000U;
    static const uint8_t alt_setting = 0x00U;

    switch (req->bmRequest & USB_REQ_TYPE_MASK) {
    case USB_REQ_TYPE_VENDOR:
        /* Microsoft WCID 驱动识别请求: Vendor Code == 'A' (0x41) */
        if (req->bRequest == 'A') {
            if ((req->wIndex & 0x00FFU) == 0x04U) {
                pbuf = USBD_WinUSB_CompatID;
                len = (req->wLength < sizeof(USBD_WinUSB_CompatID)) ? req->wLength : sizeof(USBD_WinUSB_CompatID);
            } else if ((req->wIndex & 0x00FFU) == 0x05U) {
                pbuf = USBD_WinUSB_ExtProp;
                len = (req->wLength < sizeof(USBD_WinUSB_ExtProp)) ? req->wLength : sizeof(USBD_WinUSB_ExtProp);
            }

            if (pbuf != NULL && len > 0U) {
                USBD_CtlSendData(pdev, (uint8_t *)pbuf, len);
                return USBD_OK;
            }
        }
        USBD_CtlError(pdev, req);
        return USBD_FAIL;

    case USB_REQ_TYPE_STANDARD:
        switch (req->bRequest) {
        case USB_REQ_GET_STATUS:
            USBD_CtlSendData(pdev, (uint8_t *)&status_info, 2U);
            return USBD_OK;

        case USB_REQ_GET_INTERFACE:
            USBD_CtlSendData(pdev, (uint8_t *)&alt_setting, 1U);
            return USBD_OK;

        case USB_REQ_SET_INTERFACE:
            return USBD_OK;

        default:
            USBD_CtlError(pdev, req);
            return USBD_FAIL;
        }

    case USB_REQ_TYPE_CLASS:
    default:
        USBD_CtlError(pdev, req);
        return USBD_FAIL;
    }
}

static uint8_t USBD_WinUSB_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    (void)pdev;
    if (epnum == (WINUSB_EPIN_ADDR & 0x7FU)) {
        if (g_tx_zlp) {
            g_tx_zlp = false;
            if (USBD_LL_Transmit(pdev, WINUSB_EPIN_ADDR, g_nlk_tx_buf, 0U) != USBD_OK) {
                g_tx_busy = false;
            }
        } else {
            g_tx_busy = false;
        }
    }
    return USBD_OK;
}

static uint8_t USBD_WinUSB_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    if (epnum == (WINUSB_EPOUT_ADDR & 0x7FU)) {
        uint32_t rx_cnt = USBD_LL_GetRxDataSize(pdev, WINUSB_EPOUT_ADDR);
        uint32_t i;

        for (i = 0UL; i < rx_cnt; i++) {
            if (g_nlk_rx_pos < sizeof(g_nlk_rx_buf)) {
                g_nlk_rx_buf[g_nlk_rx_pos++] = g_ep_rx_temp[i];
            }
        }

        /* Only assemble in IRQ context. Flash erase and response handling run
         * in Poll(), with OUT NAKed until the response buffer is free. */
        if (g_nlk_rx_pos >= sizeof(struct nlk_pkt_hdr)) {
            const struct nlk_pkt_hdr *hdr = (const struct nlk_pkt_hdr *)g_nlk_rx_buf;
            uint32_t total = sizeof(*hdr) + hdr->payload_len;
            if ((hdr->ver_flags & NLK_PROTO_VER_MASK) != NLK_PROTO_VER_1 ||
                total > sizeof(g_nlk_rx_buf)) {
                g_nlk_rx_pos = 0U;
            } else if (g_nlk_rx_pos >= total) {
                g_rx_ready = true;
                return USBD_OK;
            } else if (rx_cnt < WINUSB_DATA_FS_MAX_PACKET_SIZE) {
                /* A short USB transfer cannot leave a partial NLK packet. */
                g_nlk_rx_pos = 0U;
            }
        } else if (rx_cnt < WINUSB_DATA_FS_MAX_PACKET_SIZE) {
            g_nlk_rx_pos = 0U;
        }

        USBD_LL_PrepareReceive(pdev, WINUSB_EPOUT_ADDR, g_ep_rx_temp, WINUSB_DATA_FS_MAX_PACKET_SIZE);
    }
    return USBD_OK;
}

uint8_t USBD_WinUSB_Transmit(const uint8_t *buf, uint16_t len)
{
    uint8_t status = USBD_FAIL;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    USBD_HandleTypeDef *pdev = g_pusbd_dev;
    if (pdev != NULL && buf != NULL && len != 0U) {
        if (g_tx_busy) {
            status = USBD_BUSY;
        } else {
            g_tx_busy = true;
            g_tx_zlp = (len % WINUSB_DATA_FS_MAX_PACKET_SIZE) == 0U;
            if (USBD_LL_Transmit(pdev, WINUSB_EPIN_ADDR, (uint8_t *)buf, len) == USBD_OK) {
                status = USBD_OK;
            } else {
                g_tx_busy = false;
                g_tx_zlp = false;
            }
        }
    }
    __set_PRIMASK(primask);
    return status;
}

void USBD_WinUSB_Poll(void)
{
    uint32_t flag = 0UL;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (g_pusbd_dev == NULL || g_tx_busy) {
        __set_PRIMASK(primask);
        return;
    }
    if (g_reply_len != 0U) {
        if (USBD_WinUSB_Transmit(g_nlk_tx_buf, g_reply_len) == USBD_OK) {
            g_reply_len = 0U;
        }
        __set_PRIMASK(primask);
        return;
    }
    if (nlk_bl_is_reboot_pending(&flag)) {
        /* DataIn has completed both the ACK and any terminating ZLP. */
        __set_PRIMASK(primask);
        delay_ms(30U);
        nlk_bl_trigger_reboot(flag);
        return;
    }
    if (g_rx_ready) {
        const struct nlk_pkt_hdr *hdr = (const struct nlk_pkt_hdr *)g_nlk_rx_buf;
        uint16_t total = (uint16_t)(sizeof(*hdr) + hdr->payload_len);
        uint16_t remaining = g_nlk_rx_pos - total;
        uint32_t generation = g_usb_generation;
        for (uint16_t i = 0U; i < total; i++) {
            g_nlk_req_buf[i] = g_nlk_rx_buf[i];
        }
        __set_PRIMASK(primask);
        /* Never mask USB interrupts during Flash erase/program/CRC work. */
        uint16_t reply_len = nlk_bl_handle_packet(g_nlk_req_buf, total, g_nlk_tx_buf);
        __disable_irq();
        if (generation != g_usb_generation || g_pusbd_dev == NULL) {
            /* A reset may already have queued a request for the new session. */
            __set_PRIMASK(primask);
            return;
        }
        g_reply_len = reply_len;
        for (uint16_t i = 0U; i < remaining; i++) {
            g_nlk_rx_buf[i] = g_nlk_rx_buf[total + i];
        }
        g_nlk_rx_pos = remaining;
        g_rx_ready = false;
        if (remaining >= sizeof(*hdr)) {
            uint32_t next_total = sizeof(*hdr) + hdr->payload_len;
            if ((hdr->ver_flags & NLK_PROTO_VER_MASK) != NLK_PROTO_VER_1 || next_total > BL_NLK_MTU) {
                g_nlk_rx_pos = 0U;
            } else {
                g_rx_ready = remaining >= next_total;
            }
        }
        if (g_reply_len != 0U) {
            if (USBD_WinUSB_Transmit(g_nlk_tx_buf, g_reply_len) == USBD_OK) {
                g_reply_len = 0U;
            }
        }
        /* Protect both the device pointer and LL submission from reset IRQs. */
        if (!g_rx_ready) {
            USBD_LL_PrepareReceive(g_pusbd_dev, WINUSB_EPOUT_ADDR, g_ep_rx_temp, WINUSB_DATA_FS_MAX_PACKET_SIZE);
        }
    }
    __set_PRIMASK(primask);
}
