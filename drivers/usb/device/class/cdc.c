/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[USB_CDC]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sleep.h>
#include <nos/completion.h>
#include <nos/timeout.h>
#include <kernel/device.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <kernel/clk.h>
#include <string.h>
#include <lib/kfifo.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>

#include "cdc.h"

#define VCOM_INTF_STR_INDEX 5
#ifdef CONFIG_VCOM_TX_TIMEOUT
#define VCOM_TX_TIMEOUT      CONFIG_VCOM_TX_TIMEOUT
#else /*!CONFIG_VCOM_TX_TIMEOUT*/
#define VCOM_TX_TIMEOUT      1000
#endif /*CONFIG_VCOM_TX_TIMEOUT*/

#ifdef CONFIG_CDC_RX_BUFSIZE
#define CDC_RX_BUFSIZE          CONFIG_CDC_RX_BUFSIZE
#else
#define CDC_RX_BUFSIZE          128
#endif
#define CDC_MAX_PACKET_SIZE     64
#define VCOM_DEVICE             "vcom"

#ifdef CONFIG_VCOM_TX_USE_DMA
#define VCOM_TX_USE_DMA
#endif /*CONFIG_VCOM_TX_USE_DMA*/

#ifdef CONFIG_VCOM_SERNO
#define _SER_NO CONFIG_VCOM_SERNO
#else /*!CONFIG_VCOM_SERNO*/
#define _SER_NO  "32021919830108"
#endif /*CONFIG_VCOM_SERNO*/

#ifdef CONFIG_VCOM_SER_LEN
#define _SER_NO_LEN CONFIG_VCOM_SER_LEN
#else /*!CONFIG_VCOM_SER_LEN*/
#define _SER_NO_LEN 14 /*strlen("32021919830108")*/
#endif /*CONFIG_VCOM_SER_LEN*/

static struct ucdc_line_coding line_coding;

#define CDC_TX_BUFSIZE    1024
#define CDC_BULKIN_MAXSIZE (CDC_TX_BUFSIZE / 8)

#define CDC_TX_HAS_DATE   0x01
#define CDC_TX_HAS_SPACE  0x02

#ifndef kfifo_avail
#define kfifo_avail(f) kfifo_unused(f)
#endif

struct vcom {
    struct device dev;
    unsigned int mask;
    struct task_struct *cdc_task;
    struct task_struct *echo_task;
    struct ufunction *func;
    uep_t ep_out;
    uep_t ep_in;
    uep_t ep_cmd;
    struct nos_completion wait;
    struct nos_completion tx_ready;
    struct nos_completion rx_ready;
    bool connected;
    bool line_dtr;
    bool line_rts;
    bool rx_paused;
    uint8_t rx_fifo_buf[CDC_RX_BUFSIZE];
    struct kfifo rx_fifo;
    uint8_t tx_fifo_buf[CDC_TX_BUFSIZE];
    struct kfifo tx_fifo;
};

__aligned(4)
static struct udevice_descriptor dev_desc =
{
    USB_DESC_LENGTH_DEVICE,     //bLength;
    USB_DESC_TYPE_DEVICE,       //type;
    USB_BCD_VERSION,            //bcdUSB;
    USB_CLASS_CDC,              //bDeviceClass;
    0x02,                       //bDeviceSubClass;
    0x00,                       //bDeviceProtocol;
    CDC_MAX_PACKET_SIZE,        //bMaxPacketSize0;
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
    USB_CLASS_CDC,                  //bDeviceClass
    0x02,                           //bDeviceSubClass
    0x00,                           //bDeviceProtocol
    64,                             //bMaxPacketSize0
    0x01,                           //bNumConfigurations
    0,
};

/* communcation interface descriptor */
__aligned(4)
const static struct ucdc_comm_descriptor _comm_desc =
{
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    /* Interface Association Descriptor */
    {
        USB_DESC_LENGTH_IAD,
        USB_DESC_TYPE_IAD,
        USB_DYNAMIC,
        0x02,
        USB_CDC_CLASS_COMM,
        USB_CDC_SUBCLASS_ACM,
        USB_CDC_PROTOCOL_V25TER,
        0x00,
    },
#endif
    /* Interface Descriptor */
    {
        USB_DESC_LENGTH_INTERFACE,
        USB_DESC_TYPE_INTERFACE,
        USB_DYNAMIC,
        0x00,
        0x01,
        USB_CDC_CLASS_COMM,
        USB_CDC_SUBCLASS_ACM,
        USB_CDC_PROTOCOL_V25TER,
        0,
    },
    /* Header Functional Descriptor */
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_HEADER,
        0x0110,
    },
    /* Call Management Functional Descriptor */
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_CALL_MGMT,
        0x00,
        USB_DYNAMIC,
    },
    /* Abstract Control Management Functional Descriptor */
    {
        0x04,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_ACM,
        0x02,
    },
    /* Union Functional Descriptor */
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_UNION,
        USB_DYNAMIC,
        USB_DYNAMIC,
    },
    /* Endpoint Descriptor */
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_INT,
        0x08,
        0xFF,
    },
};

/* data interface descriptor */
__aligned(4)
const static struct ucdc_data_descriptor _data_desc =
{
    /* interface descriptor */
    {
        USB_DESC_LENGTH_INTERFACE,
        USB_DESC_TYPE_INTERFACE,
        USB_DYNAMIC,
        0x00,
        0x02,
        USB_CDC_CLASS_DATA,
        0x00,
        0x00,
        0x00,
    },
    /* endpoint, bulk out */
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_OUT,
        USB_EP_ATTR_BULK,
        USB_CDC_BUFSIZE,
        0x00,
    },
    /* endpoint, bulk in */
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_BULK,
        USB_CDC_BUFSIZE,
        0x00,
    },
};
__aligned(4)
static char serno[_SER_NO_LEN + 1] = {'\0'};
__weak int vcom_get_stored_serno(char *serno, int size);

int vcom_get_stored_serno(char *serno, int size)
{
    return -EFAULT;
}
__aligned(4)
const static char* _ustring[] =
{
    "Language",
    "wkcs",
    "NOS Virtual Serial",
    serno,
    "Configuration",
    "Interface",
};
static void usb_vcom_init(struct ufunction *func);

static void _vcom_reset_state(ufunction_t func)
{
    struct vcom* data;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return;
    }

    data = (struct vcom*)func->user_data;
    data->connected = false;
    data->rx_paused = false;
    kfifo_skip(&data->rx_fifo, kfifo_used(&data->rx_fifo));
    kfifo_skip(&data->tx_fifo, kfifo_used(&data->tx_fifo));
    nos_completion_reset(&data->wait);
    nos_completion_reset(&data->tx_ready);
    nos_completion_reset(&data->rx_ready);
}

/**
 * This function will handle cdc bulk in endpoint request.
 *
 * @param func the usb function object.
 * @param size request size.
 *
 * @return 0.
 */
static int _ep_in_handler(ufunction_t func, size_t size)
{
    struct vcom *data;
    size_t request_size;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }

    data = (struct vcom*)func->user_data;
    request_size = data->ep_in->request.size;
    pr_info("_ep_in_handler %d\n", request_size);
    if ((request_size != 0) && ((request_size % EP_MAXPACKET(data->ep_in)) == 0)) {
        data->ep_in->request.buffer = NULL;
        data->ep_in->request.size = 0;
        data->ep_in->request.req_type = UIO_REQUEST_WRITE;
        usbd_io_request(func->device, data->ep_in, &data->ep_in->request);

        return 0;
    }

    nos_complete(&data->wait);

    return 0;
}

/**
 * This function will handle cdc bulk out endpoint request.
 *
 * @param func the usb function object.
 * @param size request size.
 *
 * @return 0.
 */
static int _ep_out_handler(ufunction_t func, size_t size)
{
    struct vcom *data;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }

    pr_info("_ep_out_handler %d\n", size);

    data = (struct vcom*)func->user_data;
    if (size > 0) {
        kfifo_in(&data->rx_fifo, data->ep_out->buffer, (unsigned int)size);
        nos_complete(&data->rx_ready);
    }

    if (kfifo_avail(&data->rx_fifo) >= EP_MAXPACKET(data->ep_out)) {
        data->ep_out->request.buffer = data->ep_out->buffer;
        data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
        data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
        data->rx_paused = false;
        usbd_io_request(func->device, data->ep_out, &data->ep_out->request);
    } else {
        /* FIFO 空间不足，暂停挂载接收，底层硬件对主机自动回复 NAK 形成背压 */
        data->rx_paused = true;
    }

    return 0;
}

static int _ep_cmd_handler(ufunction_t func, size_t size)
{
    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }

    pr_info("_ep_cmd_handler\n");

    return 0;
}

/**
 * This function will handle cdc_get_line_coding request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return 0 on successful.
 */
static int _cdc_get_line_coding(udevice_t device, ureq_t setup)
{
    struct ucdc_line_coding data;
    uint16_t size;

    if (device == NULL) {
        pr_err("device is NULL\n");
        return -EINVAL;
    }
    if (setup == NULL) {
        pr_err("setup is NULL\n");
        return -EINVAL;
    }

    pr_info("_cdc_get_line_coding\n");

    data.dwDTERate = 115200;
    data.bCharFormat = 0;
    data.bDataBits = 8;
    data.bParityType = 0;
    size = setup->wLength > 7 ? 7 : setup->wLength;

    usbd_ep0_write(device, (void*)&data, size);

    return 0;
}

static int _cdc_set_line_coding_callback(udevice_t device, size_t size)
{
    pr_info("_cdc_set_line_coding_callback\n");

    dcd_ep0_send_status(device->dcd);

    return 0;
}

/**
 * This function will handle cdc_set_line_coding request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return 0 on successful.
 */
static int _cdc_set_line_coding(udevice_t device, ureq_t setup)
{
    if (device == NULL) {
        pr_err("device is NULL\n");
        return -EINVAL;
    }
    if (setup == NULL) {
        pr_err("setup is NULL\n");
        return -EINVAL;
    }

    pr_info("_cdc_set_line_coding\n");

    usbd_ep0_read(device, (void*)&line_coding, sizeof(struct ucdc_line_coding),
        _cdc_set_line_coding_callback);

    return 0;
}

/**
 * This function will handle cdc interface request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return 0 on successful.
 */
static int _interface_handler(ufunction_t func, ureq_t setup)
{
    struct vcom *data;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }
    if (func->device == NULL) {
        pr_err("func->device is NULL\n");
        return -EINVAL;
    }
    if (setup == NULL) {
        pr_err("setup is NULL\n");
        return -EINVAL;
    }

    data = (struct vcom*)func->user_data;

    switch(setup->bRequest) {
    case CDC_SEND_ENCAPSULATED_COMMAND:
        break;
    case CDC_GET_ENCAPSULATED_RESPONSE:
        break;
    case CDC_SET_COMM_FEATURE:
        break;
    case CDC_GET_COMM_FEATURE:
        break;
    case CDC_CLEAR_COMM_FEATURE:
        break;
    case CDC_SET_LINE_CODING:
        _cdc_set_line_coding(func->device, setup);
        break;
    case CDC_GET_LINE_CODING:
        _cdc_get_line_coding(func->device, setup);
        break;
    case CDC_SET_CONTROL_LINE_STATE:
        data->line_dtr = (setup->wValue & 0x01) > 0 ? true : false;
        data->line_rts = (setup->wValue & 0x02) > 0 ? true : false;
        pr_info("vcom line state: dtr=%d, rts=%d\n", data->line_dtr, data->line_rts);
        dcd_ep0_send_status(func->device->dcd);
        break;
    case CDC_SEND_BREAK:
        break;
    default:
        pr_err("unknown cdc request\n",setup->request_type);
        return -EINVAL;
    }

    return 0;
}

/**
 * This function will run cdc function, it will be called on handle set configuration request.
 *
 * @param func the usb function object.
 *
 * @return 0 on successful.
 */
static int _function_enable(ufunction_t func)
{
    struct vcom *data;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }

    pr_info("cdc function enable\n");

    _vcom_reset_state(func);

    data = (struct vcom*)func->user_data;
    data->connected = true;
    data->rx_paused = false;
    if (data->ep_out->buffer == NULL) {
        data->ep_out->buffer = kmalloc(CDC_RX_BUFSIZE, GFP_KERNEL);
        if (data->ep_out->buffer == NULL) {
            pr_err("alloc ep_out->buffer failed\n");
            return -EINVAL;
        }
    }

    data->ep_out->request.buffer = data->ep_out->buffer;
    data->ep_out->request.size = EP_MAXPACKET(data->ep_out);

    data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    usbd_io_request(func->device, data->ep_out, &data->ep_out->request);

    return 0;
}

/**
 * This function will stop cdc function, it will be called on handle set configuration request.
 *
 * @param func the usb function object.
 *
 * @return 0 on successful.
 */
static int _function_disable(ufunction_t func)
{
    struct vcom *data;

    if (func == NULL) {
        pr_err("func is NULL\n");
        return -EINVAL;
    }

    pr_info("cdc function disable\n");

    _vcom_reset_state(func);

    data = (struct vcom*)func->user_data;
    data->connected = false;
    data->rx_paused = false;
    if (data->ep_out->buffer != NULL) {
        kfree(data->ep_out->buffer);
        data->ep_out->buffer = NULL;
    }

    nos_complete_all(&data->rx_ready);
    nos_complete_all(&data->wait);

    return 0;
}

static struct ufunction_ops ops =
{
    _function_enable,
    _function_disable,
    NULL,
};

/**
 * This function will configure cdc descriptor.
 *
 * @param comm the communication interface number.
 * @param data the data interface number.
 *
 * @return 0 on successful.
 */
static int _cdc_descriptor_config(ucdc_comm_desc_t comm,
    uint8_t cintf_nr, ucdc_data_desc_t data, uint8_t dintf_nr)
{
    comm->call_mgmt_desc.data_interface = dintf_nr;
    comm->union_desc.master_interface = cintf_nr;
    comm->union_desc.slave_interface0 = dintf_nr;
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    comm->iad_desc.bFirstInterface = cintf_nr;
#endif

    return 0;
}

ufunction_t usbd_function_cdc_create(udevice_t device)
{
    ufunction_t func;
    struct vcom* data;
    uintf_t intf_comm, intf_data;
    ualtsetting_t comm_setting, data_setting;
    ucdc_data_desc_t data_desc;
    ucdc_comm_desc_t comm_desc;

    /* parameter check */
    if (device == NULL) {
        pr_err("device is NULL\n");
        return NULL;
    }

    memset(serno, 0, _SER_NO_LEN + 1);
    if(vcom_get_stored_serno(serno, _SER_NO_LEN) != 0)
    {
        memset(serno, 0, _SER_NO_LEN + 1);
        memcpy(serno, _SER_NO, strlen(_SER_NO));
    }

    usbd_device_set_string(device, _ustring);

    /* create a cdc function */
    func = usbd_function_new(device, &dev_desc, &ops);

    /* support HS */
    usbd_device_set_qualifier(device, &dev_qualifier);

    /* allocate memory for cdc vcom data */
    data = (struct vcom*)kmalloc(sizeof(struct vcom), GFP_KERNEL);
    if (data == NULL) {
        pr_err("no memory for vcom\n");
        return NULL;
    }
    memset(data, 0, sizeof(struct vcom));
    func->user_data = (void*)data;
    data->func = func;

    /* initilize vcom */
    usb_vcom_init(func);

    /* create a cdc communication interface and a cdc data interface */
    intf_comm = usbd_interface_new(device, _interface_handler);
    intf_data = usbd_interface_new(device, _interface_handler);

    /* create a communication alternate setting and a data alternate setting */
    comm_setting = usbd_altsetting_new(sizeof(struct ucdc_comm_descriptor));
    data_setting = usbd_altsetting_new(sizeof(struct ucdc_data_descriptor));

    /* config desc in alternate setting */
    usbd_altsetting_config_descriptor(comm_setting, &_comm_desc,
                                      (addr_t)&((ucdc_comm_desc_t)0)->intf_desc);
    usbd_altsetting_config_descriptor(data_setting, &_data_desc, 0);
    /* configure the cdc interface descriptor */
    _cdc_descriptor_config(comm_setting->desc, intf_comm->intf_num, data_setting->desc, intf_data->intf_num);

    /* create a command endpoint */
    comm_desc = (ucdc_comm_desc_t)comm_setting->desc;
    data->ep_cmd = usbd_endpoint_new(&comm_desc->ep_desc, _ep_cmd_handler);

    /* add the command endpoint to the cdc communication interface */
    usbd_altsetting_add_endpoint(comm_setting, data->ep_cmd);

    /* add the communication alternate setting to the communication interface,
       then set default setting of the interface */
    usbd_interface_add_altsetting(intf_comm, comm_setting);
    usbd_set_altsetting(intf_comm, 0);

    /* add the communication interface to the cdc function */
    usbd_function_add_interface(func, intf_comm);

    /* create a bulk in and a bulk endpoint */
    data_desc = (ucdc_data_desc_t)data_setting->desc;
    data->ep_out = usbd_endpoint_new(&data_desc->ep_out_desc, _ep_out_handler);
    data->ep_in = usbd_endpoint_new(&data_desc->ep_in_desc, _ep_in_handler);

    /* add the bulk out and bulk in endpoints to the data alternate setting */
    usbd_altsetting_add_endpoint(data_setting, data->ep_in);
    usbd_altsetting_add_endpoint(data_setting, data->ep_out);

    /* add the data alternate setting to the data interface
            then set default setting of the interface */
    usbd_interface_add_altsetting(intf_data, data_setting);
    usbd_set_altsetting(intf_data, 0);

    /* add the cdc data interface to cdc function */
    usbd_function_add_interface(func, intf_data);

    return func;
}

static ssize_t vcom_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    unsigned int read_size;
    struct vcom *data = container_of(dev, struct vcom, dev);

    (void)pos;
    if (buffer == NULL || size == 0)
        return -EINVAL;

    if (!data->connected)
        return -ENOTCONN;

    while (kfifo_used(&data->rx_fifo) == 0) {
        if (!data->connected)
            return -ENOTCONN;
        nos_completion_wait(&data->rx_ready, NOS_WAIT_FOREVER);
        if (!data->connected)
            return -ENOTCONN;
    }

    read_size = kfifo_out(&data->rx_fifo, buffer, (unsigned int)size);

    /* 背压恢复：若此前暂停了接收且现在可用空间充足，恢复挂载 */
    if (data->connected && data->rx_paused &&
        kfifo_avail(&data->rx_fifo) >= EP_MAXPACKET(data->ep_out)) {
        data->rx_paused = false;
        data->ep_out->request.buffer = data->ep_out->buffer;
        data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
        data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
        usbd_io_request(data->func->device, data->ep_out, &data->ep_out->request);
    }

    return (ssize_t)read_size;
}

static ssize_t vcom_read_timeout(struct device *dev, addr_t pos,
                                 void *buffer, size_t size, uint32_t tick)
{
    unsigned int read_size;
    struct vcom *data = container_of(dev, struct vcom, dev);
    int rc;

    (void)pos;
    if (buffer == NULL || size == 0)
        return -EINVAL;

    if (!data->connected)
        return -ENOTCONN;

    while (kfifo_used(&data->rx_fifo) == 0) {
        if (!data->connected)
            return -ENOTCONN;
        rc = nos_completion_wait(&data->rx_ready, tick);
        if (!data->connected)
            return -ENOTCONN;
        if (rc < 0 && kfifo_used(&data->rx_fifo) == 0) {
            return -ETIMEDOUT;
        }
    }

    read_size = kfifo_out(&data->rx_fifo, buffer, (unsigned int)size);

    if (data->connected && data->rx_paused &&
        kfifo_avail(&data->rx_fifo) >= EP_MAXPACKET(data->ep_out)) {
        data->rx_paused = false;
        data->ep_out->request.buffer = data->ep_out->buffer;
        data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
        data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
        usbd_io_request(data->func->device, data->ep_out, &data->ep_out->request);
    }

    return (ssize_t)read_size;
}

static ssize_t vcom_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct vcom *data = container_of(dev, struct vcom, dev);
    unsigned int written;

    (void)pos;
    if (buffer == NULL || size == 0)
        return -EINVAL;

    if (!data->connected || data->func->device->state != USB_STATE_CONFIGURED)
        return -ENOTCONN;

    written = kfifo_in(&data->tx_fifo, buffer, (unsigned int)size);
    if (written > 0) {
        nos_complete(&data->tx_ready);
    }

    return (ssize_t)written;
}

static void vcom_tx_task_entry(void* parameter)
{
    uint32_t rc;
    struct ufunction *func = (struct ufunction *)parameter;
    struct vcom *data = (struct vcom*)func->user_data;
    uint8_t ch[CDC_BULKIN_MAXSIZE];

    while (1) {
        nos_completion_wait(&data->tx_ready, NOS_WAIT_FOREVER);
        while(kfifo_used(&data->tx_fifo) > 0) {
            rc = kfifo_out(&data->tx_fifo, ch, CDC_BULKIN_MAXSIZE);
            if (rc <= 0)
                continue;
            data->ep_in->request.buffer     = ch;
            data->ep_in->request.size       = rc;
            data->ep_in->request.req_type   = UIO_REQUEST_WRITE;
            nos_completion_reset(&data->wait);
            usbd_io_request(func->device, data->ep_in, &data->ep_in->request);

            if (nos_completion_wait(&data->wait, msec_to_tick(VCOM_TX_TIMEOUT)) != 0) {
                pr_info("vcom tx timeout\n");
            }
        }
    }
}

static void vcom_rx_echo_task_entry(void *parameter)
{
    struct ufunction *func = (struct ufunction *)parameter;
    struct vcom *data = (struct vcom *)func->user_data;
    uint8_t buf[64];
    ssize_t n;

    while (1) {
        if (!data->connected) {
            msleep(100);
            continue;
        }
        n = vcom_read(&data->dev, 0, buf, sizeof(buf));
        if (n > 0) {
            vcom_write(&data->dev, 0, buf, (size_t)n);
        } else {
            msleep(10);
        }
    }
}

static void usb_vcom_init(struct ufunction *func)
{
    struct vcom *data = (struct vcom*)func->user_data;
    int rc;

    data->mask = 0x2030;
    rc = kfifo_init(&data->rx_fifo, data->rx_fifo_buf, CDC_RX_BUFSIZE, 1);
    if (rc < 0) {
        pr_info("cdc rx fifo init error, rc=%d\n", rc);
        return;
    }
    rc = kfifo_init(&data->tx_fifo, data->tx_fifo_buf, CDC_TX_BUFSIZE, 1);
    if (rc < 0) {
        pr_info("cdc tx fifo init error, rc=%d\n", rc);
        return;
    }

    nos_completion_init(&data->wait);
    nos_completion_init(&data->tx_ready);
    nos_completion_init(&data->rx_ready);

    device_init(&data->dev);
    data->dev.name = "vcom";
    data->dev.ops.read = vcom_read;
    data->dev.ops.write = vcom_write;
    data->dev.ops.read_timeout = vcom_read_timeout;
    device_register(&data->dev);

    data->cdc_task = task_create("vcom", vcom_tx_task_entry, (void *)func, 5, 1024, 5, NULL);
    if (data->cdc_task == NULL) {
        pr_err("cdc task creat err\n");
        return;
    } else {
        pr_info("cdc task creat ok\n");
    }
    task_ready(data->cdc_task);

    data->echo_task = task_create("vcom_echo", vcom_rx_echo_task_entry, (void *)func, 6, 1024, 5, NULL);
    if (data->echo_task != NULL) {
        task_ready(data->echo_task);
    }
}
struct udclass vcom_class =
{
    .usbd_function_create = usbd_function_cdc_create
};

int usbd_vcom_class_register(void)
{
    INIT_LIST_HEAD(&vcom_class.list);
    usbd_class_register(&vcom_class);
    pr_info("cdc register ok\n");
    return 0;
}
