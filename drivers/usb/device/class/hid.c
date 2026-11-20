/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[USB_HID]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <nos/msgq.h>
#include <nos/timeout.h>
#include <kernel/device.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <kernel/mutex.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>

#include "hid.h"

struct hid_s {
    struct device dev;
    struct ufunction *func;
    uep_t ep_in;
    uep_t ep_out;
    int status;
    uint16_t protocol;
    uint8_t report_buf[MAX_REPORT_SIZE];
    struct hid_report tx_report;
    struct mutex tx_lock;
    uint32_t session;
    bool tx_enabled;
    bool tx_busy;
    uint8_t set_report_type;
    uint8_t set_report_id;
    volatile uint8_t indicator_leds;
    struct nos_msgq hid_mq;
    uint8_t mq_storage[8 * sizeof(struct hid_report)];
};

static struct hid_s *g_hid_dev;

/* CustomHID_ConfigDescriptor */
__aligned(4)
const uint8_t _report_desc[]=
{
#ifdef CONFIG_USB_DEVICE_HID_KEYBOARD
    USAGE_PAGE(1),      0x01,
    USAGE(1),           0x06,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_KEYBOARD1,

    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0xE0,
    USAGE_MAXIMUM(1),   0xE7,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x08,
    INPUT(1),           0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x08,
    INPUT(1),           0x01,


    REPORT_COUNT(1),    0x05,
    REPORT_SIZE(1),     0x01,
    USAGE_PAGE(1),      0x08,
    USAGE_MINIMUM(1),   0x01,
    USAGE_MAXIMUM(1),   0x05,
    OUTPUT(1),          0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x03,
    OUTPUT(1),          0x01,


    REPORT_COUNT(1),    0x06,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x65,
    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(1),   0x65,
    INPUT(1),           0x00,
    END_COLLECTION(0),
#if USB_DEVICE_HID_KEYBOARD_NUMBER>1
    /****keyboard2*****/
    USAGE_PAGE(1),      0x01,
    USAGE(1),           0x06,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_KEYBOARD2,

    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0xE0,
    USAGE_MAXIMUM(1),   0xE7,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x08,
    INPUT(1),           0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x08,
    INPUT(1),           0x01,

    REPORT_COUNT(1),    0x06,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x65,
    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(1),   0x65,
    INPUT(1),           0x00,
    END_COLLECTION(0),
#if USB_DEVICE_HID_KEYBOARD_NUMBER>2
    USAGE_PAGE(1),      0x01,
    USAGE(1),           0x06,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_KEYBOARD3,

    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0xE0,
    USAGE_MAXIMUM(1),   0xE7,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x08,
    INPUT(1),           0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x08,
    INPUT(1),           0x01,

    REPORT_COUNT(1),    0x06,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x65,
    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(1),   0x65,
    INPUT(1),           0x00,
    END_COLLECTION(0),
#if USB_DEVICE_HID_KEYBOARD_NUMBER>3
    USAGE_PAGE(1),      0x01,
    USAGE(1),           0x06,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_KEYBOARD4,

    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0xE0,
    USAGE_MAXIMUM(1),   0xE7,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x08,
    INPUT(1),           0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x08,
    INPUT(1),           0x01,

    REPORT_COUNT(1),    0x06,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x65,
    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(1),   0x65,
    INPUT(1),           0x00,
    END_COLLECTION(0),
#endif
#endif
#endif
#endif
    // Media Control
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
    USAGE_PAGE(1),      0x0C,
    USAGE(1),           0x01,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_MEDIA,
    USAGE_PAGE(1),      0x0C,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x07,
    USAGE(1),           0xB5,             // Next Track
    USAGE(1),           0xB6,             // Previous Track
    USAGE(1),           0xB7,             // Stop
    USAGE(1),           0xCD,             // Play / Pause
    USAGE(1),           0xE2,             // Mute
    USAGE(1),           0xE9,             // Volume Up
    USAGE(1),           0xEA,             // Volume Down
    INPUT(1),           0x02,             // Input (Data, Variable, Absolute)
    REPORT_COUNT(1),    0x01,
    INPUT(1),           0x01,
    END_COLLECTION(0),
#endif

#ifdef USB_DEVICE_HID_GENERAL
    USAGE_PAGE(1),      0x8c,
    USAGE(1),           0x01,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_GENERAL,

    REPORT_COUNT(1),    RT_USB_DEVICE_HID_GENERAL_IN_REPORT_LENGTH,
    USAGE(1),           0x03,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0xFF,
    INPUT(1),           0x02,

    REPORT_COUNT(1),    RT_USB_DEVICE_HID_GENERAL_OUT_REPORT_LENGTH,
    USAGE(1),           0x04,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0xFF,
    OUTPUT(1),          0x02,
    END_COLLECTION(0),
#endif
#ifdef USB_DEVICE_HID_MOUSE
    USAGE_PAGE(1),      0x01,           // Generic Desktop
    USAGE(1),           0x02,           // Mouse
    COLLECTION(1),      0x01,           // Application
    USAGE(1),           0x01,           // Pointer
    COLLECTION(1),      0x00,           // Physical
    REPORT_ID(1),       HID_REPORT_ID_MOUSE,
    REPORT_COUNT(1),    0x03,
    REPORT_SIZE(1),     0x01,
    USAGE_PAGE(1),      0x09,           // Buttons
    USAGE_MINIMUM(1),   0x1,
    USAGE_MAXIMUM(1),   0x3,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    INPUT(1),           0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x05,
    INPUT(1),           0x01,
    REPORT_COUNT(1),    0x03,
    REPORT_SIZE(1),     0x08,
    USAGE_PAGE(1),      0x01,
    USAGE(1),           0x30,           // X
    USAGE(1),           0x31,           // Y
    USAGE(1),           0x38,           // scroll
    LOGICAL_MINIMUM(1), 0x81,
    LOGICAL_MAXIMUM(1), 0x7f,
    INPUT(1),           0x06,
    END_COLLECTION(0),
    END_COLLECTION(0),
#endif
}; /* CustomHID_ReportDescriptor */

__aligned(4)
static struct udevice_descriptor _dev_desc =
{
    USB_DESC_LENGTH_DEVICE,     //bLength;
    USB_DESC_TYPE_DEVICE,       //type;
    USB_BCD_VERSION,            //bcdUSB;
    USB_CLASS_HID,              //bDeviceClass;
    0x00,                       //bDeviceSubClass;
    0x00,                       //bDeviceProtocol;
    64,                         //bMaxPacketSize0;
    _VENDOR_ID,                 //idVendor;
    _PRODUCT_ID,                //idProduct;
    USB_BCD_DEVICE,             //bcdDevice;
    USB_STRING_MANU_INDEX,      //iManufacturer;
    USB_STRING_PRODUCT_INDEX,   //iProduct;
    USB_STRING_SERIAL_INDEX,    //iSerialNumber;
    USB_DYNAMIC,                //bNumConfigurations;
};

//FS and HS needed
__aligned(4)
static struct usb_qualifier_descriptor dev_qualifier =
{
    sizeof(dev_qualifier),          //bLength
    USB_DESC_TYPE_DEVICEQUALIFIER,  //bDescriptorType
    0x0200,                         //bcdUSB
    USB_CLASS_MASS_STORAGE,         //bDeviceClass
    0x06,                           //bDeviceSubClass
    0x50,                           //bDeviceProtocol
    64,                             //bMaxPacketSize0
    0x01,                           //bNumConfigurations
    0,
};


/* hid interface descriptor */
__aligned(4)
static const struct uhid_comm_descriptor _hid_comm_desc =
{
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    /* Interface Association Descriptor */
    {
        USB_DESC_LENGTH_IAD,
        USB_DESC_TYPE_IAD,
        USB_DYNAMIC,
        0x01,
        0x03,                       /* bInterfaceClass: HID */
#if !defined(CONFIG_USB_DEVICE_HID_MEDIA) && (defined(CONFIG_USB_DEVICE_HID_KEYBOARD) || defined(USB_DEVICE_HID_MOUSE))
        USB_HID_SUBCLASS_BOOT,    /* bInterfaceSubClass : 1=BOOT, 0=no boot */
#else
        USB_HID_SUBCLASS_NOBOOT,    /* bInterfaceSubClass : 1=BOOT, 0=no boot */
#endif
#if !defined(CONFIG_USB_DEVICE_HID_KEYBOARD)||!defined(USB_DEVICE_HID_MOUSE)||!defined(CONFIG_USB_DEVICE_HID_MEDIA)
        USB_HID_PROTOCOL_NONE,      /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#elif !defined(USB_DEVICE_HID_MOUSE)
        USB_HID_PROTOCOL_KEYBOARD,  /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#else
        USB_HID_PROTOCOL_MOUSE,     /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#endif
        0x00,
    },
#endif
    /* Interface Descriptor */
    {
        USB_DESC_LENGTH_INTERFACE,
        USB_DESC_TYPE_INTERFACE,
        USB_DYNAMIC,                /* bInterfaceNumber: Number of Interface */
        0x00,                       /* bAlternateSetting: Alternate setting */
        0x02,                       /* bNumEndpoints */
        0x03,                       /* bInterfaceClass: HID */
#if !defined(CONFIG_USB_DEVICE_HID_MEDIA) && (defined(CONFIG_USB_DEVICE_HID_KEYBOARD) || defined(USB_DEVICE_HID_MOUSE))
        USB_HID_SUBCLASS_BOOT,    /* bInterfaceSubClass : 1=BOOT, 0=no boot */
#else
        USB_HID_SUBCLASS_NOBOOT,    /* bInterfaceSubClass : 1=BOOT, 0=no boot */
#endif
#if !defined(CONFIG_USB_DEVICE_HID_KEYBOARD)||!defined(USB_DEVICE_HID_MOUSE)||!defined(CONFIG_USB_DEVICE_HID_MEDIA)
        USB_HID_PROTOCOL_NONE,      /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#elif !defined(USB_DEVICE_HID_MOUSE)
        USB_HID_PROTOCOL_KEYBOARD,  /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#else
        USB_HID_PROTOCOL_MOUSE,     /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */
#endif
        0,                          /* iInterface: Index of string descriptor */
    },

    /* HID Descriptor */
    {
        HID_DESCRIPTOR_SIZE,        /* bLength: HID Descriptor size */
        HID_DESCRIPTOR_TYPE,        /* bDescriptorType: HID */
        0x0110,                     /* bcdHID: HID Class Spec release number */
        0x00,                       /* bCountryCode: Hardware target country */
        0x01,                       /* bNumDescriptors: Number of HID class descriptors to follow */
        {
            {
                0x22,                       /* bDescriptorType */
                sizeof(_report_desc),       /* wItemLength: Total length of Report descriptor */
            },
        },
    },

    /* Endpoint Descriptor IN */
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_INT,
        0x40,
        0x01,
    },

    /* Endpoint Descriptor OUT */
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_OUT,
        USB_EP_ATTR_INT,
        0x40,
        0x01,
    },
};

__aligned(4)
static const char* _ustring[] =
{
    "Language",
    "wkcs",
    "NOS HID-Device",
    "32021919830108",
    "Configuration",
    "Interface",
};

static void dump_data(uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++)
    {
        pr_info("%02x\n", *data++);
    }
}
static void dump_report(struct hid_report * report)
{
    pr_info("HID Recived:\n");
    pr_info("Report ID %02x\n", report->report_id);
    dump_data(report->report,report->size);
}

/* 从主机下发的 Output Report 中提取标准 5bit 键盘指示灯状态（Num/Caps/Scroll/Compose/Kana） */
static void _hid_update_leds(struct hid_s *data, uint8_t report_id, const uint8_t *buf, size_t size)
{
    uint8_t leds;

    if (data == NULL || buf == NULL || size == 0U) {
        return;
    }
    if (report_id != HID_REPORT_ID_KEYBOARD1 && report_id != 0U) {
        return;
    }
    if (size >= 2U && buf[0] == HID_REPORT_ID_KEYBOARD1) {
        leds = buf[1] & 0x1FU;
    } else if (size == 1U) {
        leds = buf[0] & 0x1FU;
    } else if (report_id == HID_REPORT_ID_KEYBOARD1) {
        /* 某些主机在 wValue 指定了 Report ID 但 data stage 发送多字节未带前缀时取首字节 */
        leds = buf[0] & 0x1FU;
    } else {
        return;
    }
    __atomic_store_n(&data->indicator_leds, leds, __ATOMIC_RELEASE);
}

static int _ep_out_handler(ufunction_t func, size_t size)
{
    struct hid_s *data;
    struct hid_report report;

    data = (struct hid_s *) func->user_data;

    if (size != 0) {
        const uint8_t *raw = (const uint8_t *)data->ep_out->buffer;
        if (size >= 2U && raw[0] == HID_REPORT_ID_KEYBOARD1) {
            _hid_update_leds(data, HID_REPORT_ID_KEYBOARD1, raw, size);
        } else if (size == 1U && data->protocol == 0U) {
            _hid_update_leds(data, 0U, raw, size);
        }
        memcpy((void *)&report,(void*)data->ep_out->buffer, size);
        report.size = size-1;
        nos_msgq_send(&data->hid_mq, &report, sizeof(report), NOS_NO_WAIT);
    }

    data->ep_out->request.buffer = data->ep_out->buffer;
    data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
    data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    usbd_io_request(func->device, data->ep_out, &data->ep_out->request);
    return 0;
}

static int _ep_in_handler(ufunction_t func, size_t size)
{
    struct hid_s *data = func->user_data;

    (void)size;
    mutex_lock(&data->tx_lock);
    data->tx_busy = false;
    mutex_unlock(&data->tx_lock);
    return 0;
}

static int _hid_set_report_callback(udevice_t device, size_t size)
{
    if (g_hid_dev != NULL && g_hid_dev->func != NULL && g_hid_dev->func->device == device &&
        size != 0U && g_hid_dev->set_report_type == HID_REPORT_OUTPUT) {
        _hid_update_leds(g_hid_dev, g_hid_dev->set_report_id, g_hid_dev->report_buf, size);
    }

    dcd_ep0_send_status(device->dcd);

    return 0;
}

/**
 * This function will handle hid interface bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _interface_handler(ufunction_t func, ureq_t setup)
{
    struct hid_s *data = (struct hid_s *) func->user_data;

    /* if(setup->wIndex != 0) {
        return -EIO;
    } */

    switch (setup->bRequest)
    {
    case USB_REQ_GET_DESCRIPTOR:
        if((setup->wValue >> 8) == USB_DESC_TYPE_REPORT)
        {
            usbd_ep0_write(func->device, (void *)(&_report_desc[0]), sizeof(_report_desc));
        }
        else if((setup->wValue >> 8) == USB_DESC_TYPE_HID)
        {

            usbd_ep0_write(func->device, (void *)(&_hid_comm_desc.hid_desc), sizeof(struct uhid_descriptor));
        }
        break;
    case USB_HID_REQ_GET_REPORT:
        if(setup->wLength == 0)
        {
            usbd_ep0_set_stall(func->device);
            break;
        }
        if((setup->wLength == 0) || (setup->wLength > MAX_REPORT_SIZE))
            setup->wLength = MAX_REPORT_SIZE;
        usbd_ep0_write(func->device, data->report_buf,setup->wLength);
        break;
    case USB_HID_REQ_GET_IDLE:

        dcd_ep0_send_status(func->device->dcd);
        break;
    case USB_HID_REQ_GET_PROTOCOL:
        usbd_ep0_write(func->device, &data->protocol,2);
        break;
    case USB_HID_REQ_SET_REPORT:

        if ((setup->wLength == 0) || (setup->wLength > MAX_REPORT_SIZE)) {
            usbd_ep0_set_stall(func->device);
            break;
        }
        data->set_report_type = (uint8_t)(setup->wValue >> 8);
        data->set_report_id = (uint8_t)(setup->wValue & 0xFFU);

        usbd_ep0_read(func->device, data->report_buf, setup->wLength, _hid_set_report_callback);
        break;
    case USB_HID_REQ_SET_IDLE:
        dcd_ep0_send_status(func->device->dcd);
        break;
    case USB_HID_REQ_SET_PROTOCOL:
        data->protocol = setup->wValue;

        dcd_ep0_send_status(func->device->dcd);
        break;
    }

    return 0;
}


/**
 * This function will run cdc function, it will be called on handle set configuration bRequest.
 *
 * @param func the usb function object.
 *
 * @return 0 on successful.
 */
static int _function_enable(ufunction_t func)
{
    struct hid_s *data;

    data = (struct hid_s *) func->user_data;

    pr_info("hid function enable\n");
//
//    _vcom_reset_state(func);
//
    if(data->ep_out->buffer == NULL)
    {
        data->ep_out->buffer        = kmalloc(HID_RX_BUFSIZE, GFP_KERNEL);
    }
    data->ep_out->request.buffer    = data->ep_out->buffer;
    data->ep_out->request.size      = EP_MAXPACKET(data->ep_out);
    data->ep_out->request.req_type  = UIO_REQUEST_READ_BEST;

    usbd_io_request(func->device, data->ep_out, &data->ep_out->request);

    __atomic_store_n(&data->indicator_leds, 0U, __ATOMIC_RELEASE);
    mutex_lock(&data->tx_lock);
    data->session++;
    data->tx_busy = false;
    data->tx_enabled = true;
    mutex_unlock(&data->tx_lock);
    return 0;
}

/**
 * This function will stop cdc function, it will be called on handle set configuration bRequest.
 *
 * @param func the usb function object.
 *
 * @return 0 on successful.
 */
static int _function_disable(ufunction_t func)
{
    struct hid_s *data;

    data = (struct hid_s *) func->user_data;

    pr_info("hid function disable\n");

    __atomic_store_n(&data->indicator_leds, 0U, __ATOMIC_RELEASE);
    mutex_lock(&data->tx_lock);
    data->tx_enabled = false;
    data->tx_busy = false;
    mutex_unlock(&data->tx_lock);
    if(data->ep_out->buffer != NULL)
    {
        kfree(data->ep_out->buffer);
        data->ep_out->buffer = NULL;
    }

    return 0;
}

static struct ufunction_ops hid_function_ops =
{
    _function_enable,
    _function_disable,
    NULL,
};




/**
 * This function will configure hid descriptor.
 *
 * @param comm the communication interface number.
 * @param data the data interface number.
 *
 * @return 0 on successful.
 */
static int _hid_descriptor_config(__maybe_unused uhid_comm_desc_t hid, __maybe_unused uint8_t cintf_nr)
{
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    hid->iad_desc.bFirstInterface = cintf_nr;
#endif

    return 0;
}
static ssize_t _hid_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct hid_s *hid_dev = container_of(dev, struct hid_s, dev);
    udevice_t device = hid_dev->func->device;
    uep_t ep = hid_dev->ep_in;
    ssize_t ret;

    if (buffer == NULL || size == 0U || size > sizeof(hid_dev->tx_report.report) ||
        size + 1U > EP_MAXPACKET(ep)) {
        return -EINVAL;
    }

    mutex_lock(&hid_dev->tx_lock);
    if (!hid_dev->tx_enabled || device->state != USB_STATE_CONFIGURED) {
        ret = -ENOTCONN;
    } else if (hid_dev->tx_busy || ep->stalled) {
        ret = -EAGAIN;
    } else {
        hid_dev->tx_report.report_id = pos;
        memcpy(hid_dev->tx_report.report, buffer, size);
        ep->request.buffer = (void *)&hid_dev->tx_report;
        ep->request.size = size + 1U;
        ep->request.req_type = UIO_REQUEST_WRITE;
        hid_dev->tx_busy = true;
        if (usbd_io_request(device, ep, &ep->request) == size + 1U) {
            ret = size;
        } else {
            hid_dev->tx_busy = false;
            ret = -EAGAIN;
        }
    }
    mutex_unlock(&hid_dev->tx_lock);
    return ret;
}

static int _hid_control(struct device *dev, int cmd, void *arg)
{
    struct hid_s *hid_dev = container_of(dev, struct hid_s, dev);
    int ret = -ENOTCONN;

    if (arg == NULL || (cmd != USB_HID_CTRL_GET_SESSION && cmd != USB_HID_CTRL_GET_INDICATORS)) {
        return -EINVAL;
    }
    mutex_lock(&hid_dev->tx_lock);
    if (hid_dev->tx_enabled && hid_dev->func->device->state == USB_STATE_CONFIGURED) {
        if (cmd == USB_HID_CTRL_GET_SESSION) {
            *(uint32_t *)arg = hid_dev->session;
        } else {
            *(uint8_t *)arg = __atomic_load_n(&hid_dev->indicator_leds, __ATOMIC_ACQUIRE);
        }
        ret = 0;
    }
    mutex_unlock(&hid_dev->tx_lock);
    return ret;
}
__weak void HID_Report_Received(hid_report_t report)
{
    dump_report(report);
}

static struct task_struct *hid_task;

static void hid_task_entry(void* parameter)
{
    struct hid_report report;
    struct hid_s *hiddev;
    hiddev = (struct hid_s *)parameter;
    while (1) {
        if (nos_msgq_recv(&hiddev->hid_mq, &report, sizeof(struct hid_report), NOS_WAIT_FOREVER) != 0)
            continue;
        HID_Report_Received(&report);
    }
}

static void usb_hid_init(struct ufunction *func)
{
    struct hid_s *hiddev;
    hiddev = (struct hid_s *)func->user_data;

    hiddev->func = func;
    g_hid_dev = hiddev;

    device_init(&hiddev->dev);
    hiddev->dev.name = "hidd";
    hiddev->dev.ops.write = _hid_write;
    hiddev->dev.ops.control = _hid_control;
    mutex_init(&hiddev->tx_lock);
    device_register(&hiddev->dev);

    hid_task = task_create("hidd", hid_task_entry, (void *)hiddev, 5, 1024, 5, NULL);
    if (hid_task == NULL) {
        pr_err("hid task creat err\n");
        return;
    } else {
        pr_info("hid task creat ok\n");
    }
    nos_msgq_init(&hiddev->hid_mq, hiddev->mq_storage, sizeof(hiddev->mq_storage), sizeof(struct hid_report), 8);
    task_ready(hid_task);
}


/**
 * This function will create a hid function instance.
 *
 * @param device the usb device object.
 *
 * @return 0 on successful.
 */
ufunction_t usbd_function_hid_create(udevice_t device)
{
    ufunction_t     func;
    struct hid_s   *data;

    uintf_t         hid_intf;
    ualtsetting_t   hid_setting;
    uhid_comm_desc_t hid_desc;

    /* parameter check */

    /* set usb device string description */
    usbd_device_set_string(device, _ustring);

    /* create a cdc function */
    func = usbd_function_new(device, &_dev_desc, &hid_function_ops);
    //not support hs
    //usbd_device_set_qualifier(device, &_dev_qualifier);

    /* allocate memory for cdc vcom data */
    data = (struct hid_s*)kmalloc(sizeof(struct hid_s), GFP_KERNEL);
    memset(data, 0, sizeof(struct hid_s));
    func->user_data = (void*)data;

    /* create an interface object */
    hid_intf = usbd_interface_new(device, _interface_handler);
    pr_info("hid_intf->intf_num = %d\n", hid_intf->intf_num);

    /* create an alternate setting object */
    hid_setting = usbd_altsetting_new(sizeof(struct uhid_comm_descriptor));

    /* config desc in alternate setting */
    usbd_altsetting_config_descriptor(hid_setting, &_hid_comm_desc, (addr_t)&((uhid_comm_desc_t)0)->intf_desc);

    /* configure the hid interface descriptor */
    _hid_descriptor_config(hid_setting->desc, hid_intf->intf_num);

    /* create endpoint */
    hid_desc = (uhid_comm_desc_t)hid_setting->desc;
    data->ep_out = usbd_endpoint_new(&hid_desc->ep_out_desc, _ep_out_handler);
    data->ep_in  = usbd_endpoint_new(&hid_desc->ep_in_desc, _ep_in_handler);

    /* add the int out and int in endpoint to the alternate setting */
    usbd_altsetting_add_endpoint(hid_setting, data->ep_out);
    usbd_altsetting_add_endpoint(hid_setting, data->ep_in);

    /* add the alternate setting to the interface, then set default setting */
    usbd_interface_add_altsetting(hid_intf, hid_setting);
    usbd_set_altsetting(hid_intf, 0);

    /* add the interface to the mass storage function */
    usbd_function_add_interface(func, hid_intf);

    /* initilize hid */
    usb_hid_init(func);
    return func;
}
struct udclass hid_class =
{
    .usbd_function_create = usbd_function_hid_create,
};

int usbd_hid_class_register(void)
{
    INIT_LIST_HEAD(&hid_class.list);
    usbd_class_register(&hid_class);
    pr_info("hid register ok\n");
    return 0;
}
