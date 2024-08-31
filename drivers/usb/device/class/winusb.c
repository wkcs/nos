/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <sys/types.h>
#define pr_fmt(fmt) "[WINUSB]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/task.h>
#include <kernel/sem.h>
#include <kernel/device.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <usb/usb_common.h>
#include <usb/usb_device.h>

#include "winusb.h"

void winusb_read_test(void);

struct winusb_device {
    struct device dev;
    void (*cmd_handler)(uint8_t *buffer, size_t size);
    struct ufunction *func;
    uint8_t cmd_buff[1024];
    uep_t ep_out;
    uep_t ep_in;
    sem_t read_sem;
    sem_t write_sem;
};

struct winusb_device *winusb_dev;

__aligned(4)
static struct udevice_descriptor dev_desc = {
    USB_DESC_LENGTH_DEVICE,     //bLength;
    USB_DESC_TYPE_DEVICE,       //type;
    USB_BCD_VERSION,            //bcdUSB;
    0x00,                       //bDeviceClass;
    0x00,                       //bDeviceSubClass;
    0x00,                       //bDeviceProtocol;
    0x40,                       //bMaxPacketSize0;
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
static struct usb_qualifier_descriptor dev_qualifier = {
    sizeof(dev_qualifier),          //bLength
    USB_DESC_TYPE_DEVICEQUALIFIER,  //bDescriptorType
    0x0200,                         //bcdUSB
    0xFF,                           //bDeviceClass
    0x00,                           //bDeviceSubClass
    0x00,                           //bDeviceProtocol
    64,                             //bMaxPacketSize0
    0x01,                           //bNumConfigurations
    0,
};

__aligned(4)
struct winusb_descriptor _winusb_desc = {
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    /* Interface Association Descriptor */
    {
        USB_DESC_LENGTH_IAD,
        USB_DESC_TYPE_IAD,
        USB_DYNAMIC,
        0x01,
        0xFF,
        0x00,
        0x00,
        0x00,
    },
#endif
    /*interface descriptor*/
    {
        USB_DESC_LENGTH_INTERFACE,  //bLength;
        USB_DESC_TYPE_INTERFACE,    //type;
        USB_DYNAMIC,                //bInterfaceNumber;
        0x00,                       //bAlternateSetting;
        0x02,                       //bNumEndpoints
        0xFF,                       //bInterfaceClass;
        0x00,                       //bInterfaceSubClass;
        0x00,                       //bInterfaceProtocol;
        0x00,                       //iInterface;
    },
    /*endpoint descriptor*/
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_OUT,
        USB_EP_ATTR_BULK,
        USB_DYNAMIC,
        0x00,
    },
    /*endpoint descriptor*/
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_BULK,
        USB_DYNAMIC,
        0x00,
    },
};

__aligned(4)
static const char* _ustring[] =
{
    "Language",
    "wkcs",
    "NOS Win USB",
    "32021919830108",
    "Configuration",
    "Interface",
    USB_STRING_OS//must be
};

__aligned(4)
struct usb_os_proerty winusb_proerty[] =
{
    USB_OS_PROERTY_DESC(USB_OS_PROERTY_TYPE_REG_SZ,"DeviceInterfaceGUID", WINUSB_GUID),
};

__aligned(4)
struct usb_os_function_comp_id_descriptor winusb_func_comp_id_desc =
{
    .bFirstInterfaceNumber = USB_DYNAMIC,
    .reserved1          = 0x01,
    .compatibleID       = {'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00},
    .subCompatibleID    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    .reserved2          = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

static int _ep_out_handler(ufunction_t func, size_t size)
{
    struct winusb_device *winusb_device = (struct winusb_device *)func->user_data;

    sem_send_one(&winusb_device->read_sem);

    return 0;
}

static int _ep_in_handler(ufunction_t func, size_t size)
{
    struct winusb_device *winusb_device = (struct winusb_device *)func->user_data;

    sem_send_one(&winusb_device->write_sem);

    return 0;
}
static ufunction_t cmd_func = NULL;
static int _ep0_cmd_handler(udevice_t device, size_t size)
{
    struct winusb_device *winusb_device;

    if(cmd_func != NULL) {
        winusb_device = (struct winusb_device *)cmd_func->user_data;
        cmd_func = NULL;
        if (winusb_device->cmd_handler != NULL)
            winusb_device->cmd_handler(winusb_device->cmd_buff, size);

    }
    dcd_ep0_send_status(device->dcd);
    return 0;
}
static int _ep0_cmd_read(ufunction_t func, ureq_t setup)
{
    struct winusb_device *winusb_device = (struct winusb_device *)func->user_data;
    cmd_func = func;
    usbd_ep0_read(func->device, winusb_device->cmd_buff, setup->wLength, _ep0_cmd_handler);
    return 0;
}
static int _interface_handler(ufunction_t func, ureq_t setup)
{
    switch (setup->bRequest) {
    case 'A':
        switch (setup->wIndex) {
        case 0x05:
            usbd_os_proerty_descriptor_send(func, setup, winusb_proerty,
                                            sizeof(winusb_proerty) / sizeof(winusb_proerty[0]));
            break;
        }
        break;
    case 0x0A://customer
        _ep0_cmd_read(func, setup);
        break;
    }

    return 0;
}
static int _function_enable(ufunction_t func)
{
    return 0;
}
static int _function_disable(ufunction_t func)
{
    return 0;
}

static struct ufunction_ops ops =
{
    _function_enable,
    _function_disable,
    NULL,
};

static int _winusb_descriptor_config(winusb_desc_t winusb, uint8_t cintf_nr, uint8_t device_is_hs)
{
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    winusb->iad_desc.bFirstInterface = cintf_nr;
#endif
    winusb->ep_out_desc.wMaxPacketSize = device_is_hs ? 512 : 64;
    winusb->ep_in_desc.wMaxPacketSize = device_is_hs ? 512 : 64;
    winusb_func_comp_id_desc.bFirstInterfaceNumber = cintf_nr;
    return 0;
}

static ssize_t winusb_read(struct device *dev, addr_t pos,
    void *buffer, size_t size)
{
    size_t read_size;
    struct winusb_device *win_dev = container_of(dev, struct winusb_device, dev);

    if (win_dev->func->device->state != USB_STATE_CONFIGURED)
        return 0;

    win_dev->ep_out->buffer = buffer;
    win_dev->ep_out->request.buffer = buffer;
    win_dev->ep_out->request.size = size;
    win_dev->ep_out->request.req_type = UIO_REQUEST_READ_FULL;
    read_size = usbd_io_request(win_dev->func->device, win_dev->ep_out,
        &win_dev->ep_out->request);

    sem_get(&win_dev->read_sem);

    return read_size;
}

static ssize_t winusb_write(struct device *dev, addr_t pos,
    const void *buffer, size_t size)
{
    size_t write_size;
    struct winusb_device *win_dev = container_of(dev, struct winusb_device, dev);

    if (win_dev->func->device->state != USB_STATE_CONFIGURED)
        return 0;

    win_dev->ep_in->buffer = (void *)buffer;
    win_dev->ep_in->request.buffer = win_dev->ep_in->buffer;
    win_dev->ep_in->request.size = size;
    win_dev->ep_in->request.req_type = UIO_REQUEST_WRITE;
    write_size = usbd_io_request(win_dev->func->device, win_dev->ep_in,
        &win_dev->ep_in->request);

    sem_get(&win_dev->write_sem);

    return write_size;
}

static ssize_t winusb_read_timeout(struct device *dev, addr_t pos,
    void *buffer, size_t size, uint32_t tick)
{
    size_t read_size;
    struct winusb_device *win_dev =
        container_of(dev, struct winusb_device, dev);
    int rc;

    if (win_dev->func->device->state != USB_STATE_CONFIGURED)
        return 0;

    win_dev->ep_out->buffer = buffer;
    win_dev->ep_out->request.buffer = buffer;
    win_dev->ep_out->request.size = size;
    win_dev->ep_out->request.req_type = UIO_REQUEST_READ_FULL;
    read_size = usbd_io_request(win_dev->func->device, win_dev->ep_out,
        &win_dev->ep_out->request);

    rc = sem_get_timeout(&win_dev->read_sem, tick);
    if (rc < 0) {
        pr_err("read timeout\r\n");
        return -ETIMEDOUT;
    }

    return read_size;
}

static ssize_t winusb_write_timeout(struct device *dev, addr_t pos,
    const void *buffer, size_t size, uint32_t tick)
{
    size_t write_size;
    struct winusb_device *win_dev =
        container_of(dev, struct winusb_device, dev);
    int rc;

    if (win_dev->func->device->state != USB_STATE_CONFIGURED)
        return 0;

    win_dev->ep_in->buffer = (void *)buffer;
    win_dev->ep_in->request.buffer = win_dev->ep_in->buffer;
    win_dev->ep_in->request.size = size;
    win_dev->ep_in->request.req_type = UIO_REQUEST_WRITE;
    write_size = usbd_io_request(win_dev->func->device, win_dev->ep_in,
        &win_dev->ep_in->request);

    rc = sem_get_timeout(&win_dev->write_sem, tick);
    if (rc < 0) {
        pr_err("write timeout\r\n");
        return -ETIMEDOUT;
    }

    return write_size;
}

static int  winusb_control(struct device *dev, int cmd, void *args)
{
    struct winusb_device *win_dev = container_of(dev, struct winusb_device, dev);
    if (cmd == 0x03 && win_dev->cmd_handler == NULL)
        win_dev->cmd_handler = (void(*)(uint8_t*, size_t))args;

    return 0;
}

static int usb_winusb_init(ufunction_t func)
{
    struct winusb_device *winusb_device   = (struct winusb_device *)func->user_data;

    winusb_device->func = func;

    device_init(&winusb_device->dev);
    winusb_device->dev.name = "winusb";
    winusb_device->dev.ops.read = winusb_read;
    winusb_device->dev.ops.write = winusb_write;
    winusb_device->dev.ops.read_timeout = winusb_read_timeout;
    winusb_device->dev.ops.write_timeout = winusb_write_timeout;
    winusb_device->dev.ops.control = winusb_control;
    device_register(&winusb_device->dev);

    return 0;
}

ufunction_t usbd_function_winusb_create(udevice_t device)
{
    ufunction_t         func;
    struct winusb_device *winusb_device;

    uintf_t             winusb_intf;
    ualtsetting_t       winusb_setting;
    winusb_desc_t       winusb_desc;

    /* set usb device string description */
    usbd_device_set_string(device, _ustring);

    /* create a cdc function */
    func = usbd_function_new(device, &dev_desc, &ops);
    usbd_device_set_qualifier(device, &dev_qualifier);

    /* allocate memory for cdc vcom data */
    winusb_device = kmalloc(sizeof(*winusb_device), GFP_KERNEL);
    winusb_dev = winusb_device;
    memset((void *)winusb_device, 0, sizeof(*winusb_device));
    sem_init(&winusb_device->read_sem, 0);
    sem_init(&winusb_device->write_sem, 0);
    func->user_data = (void*)winusb_device;
    /* create an interface object */
    winusb_intf = usbd_interface_new(device, _interface_handler);

    /* create an alternate setting object */
    winusb_setting = usbd_altsetting_new(sizeof(struct winusb_descriptor));

    /* config desc in alternate setting */
    usbd_altsetting_config_descriptor(winusb_setting, &_winusb_desc, (addr_t)&((winusb_desc_t)0)->intf_desc);

    /* configure the hid interface descriptor */
    _winusb_descriptor_config(winusb_setting->desc, winusb_intf->intf_num, device->dcd->device_is_hs);

    /* create endpoint */
    winusb_desc = (winusb_desc_t)winusb_setting->desc;
    winusb_device->ep_out = usbd_endpoint_new(&winusb_desc->ep_out_desc, _ep_out_handler);
    winusb_device->ep_in  = usbd_endpoint_new(&winusb_desc->ep_in_desc, _ep_in_handler);

    /* add the int out and int in endpoint to the alternate setting */
    usbd_altsetting_add_endpoint(winusb_setting, winusb_device->ep_out);
    usbd_altsetting_add_endpoint(winusb_setting, winusb_device->ep_in);

    /* add the alternate setting to the interface, then set default setting */
    usbd_interface_add_altsetting(winusb_intf, winusb_setting);
    usbd_set_altsetting(winusb_intf, 0);

    /* add the interface to the mass storage function */
    usbd_function_add_interface(func, winusb_intf);

    usbd_os_comp_id_desc_add_os_func_comp_id_desc(device->os_comp_id_desc, &winusb_func_comp_id_desc);
    /* initilize winusb */
    usb_winusb_init(func);

    return func;
}

struct udclass winusb_class =
{
    .usbd_function_create = usbd_function_winusb_create
};

int usbd_winusb_class_register(void)
{
    INIT_LIST_HEAD(&winusb_class.list);
    usbd_class_register(&winusb_class);
    pr_info("winusb register ok\r\n");
    return 0;
}
