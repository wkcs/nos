/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[USB_DEVICE]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/printk.h>
#include <usb/usb_common.h>
#include <usb/usb_device.h>

#ifdef CONFIG_USB_DEVICE_COMPOSITE
static const char* ustring[] =
{
    "Language",
    "wkcs",
    "NOS Composite Device",
    "320219198301",
    "Configuration",
    "Interface",
    USB_STRING_OS
};

static struct udevice_descriptor compsit_desc =
{
    USB_DESC_LENGTH_DEVICE,     //bLength;
    USB_DESC_TYPE_DEVICE,       //type;
    USB_BCD_VERSION,            //bcdUSB;
    USB_CLASS_MISC,             //bDeviceClass;
    0x02,                       //bDeviceSubClass;
    0x01,                       //bDeviceProtocol;
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
static struct usb_qualifier_descriptor dev_qualifier =
{
    sizeof(dev_qualifier),          //bLength
    USB_DESC_TYPE_DEVICEQUALIFIER,  //bDescriptorType
    0x0200,                         //bcdUSB
    USB_CLASS_MISC,                 //bDeviceClass
    0x02,                           //bDeviceSubClass
    0x01,                           //bDeviceProtocol
    64,                             //bMaxPacketSize0
    0x01,                           //bNumConfigurations
    0,
};
#endif

struct usb_os_comp_id_descriptor usb_comp_id_desc =
{
    //head section
    {
        USB_DYNAMIC,
        0x0100,
        0x04,
        USB_DYNAMIC,
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    },
    {},
};

void usbd_os_proerty_descriptor_send(ufunction_t func, ureq_t setup,
                                     usb_os_proerty_t usb_os_proerty,
                                     uint8_t number_of_proerty)
{
    struct usb_os_property_header header;
    static uint8_t * data;
    uint8_t * pdata;
    uint8_t index,i;
    if(data == NULL)
    {
        header.dwLength = sizeof(struct usb_os_property_header);
        header.bcdVersion = 0x0100;
        header.wIndex = 0x05;
        header.wCount = number_of_proerty;
        for(index = 0;index < number_of_proerty;index++)
        {
            header.dwLength += usb_os_proerty[index].dwSize;
        }
        data = (uint8_t *)kmalloc(header.dwLength, GFP_KERNEL);
        // WK_ERROR(data != NULL);
        pdata = data;
        memcpy((void *)pdata,(void *)&header,sizeof(struct usb_os_property_header));
        pdata += sizeof(struct usb_os_property_header);
        for(index = 0;index < number_of_proerty;index++)
        {
            memcpy((void *)pdata,(void *)&usb_os_proerty[index],10);
            pdata += 10;
            for(i = 0;i < usb_os_proerty[index].wPropertyNameLength/2;i++)
            {
                *pdata = usb_os_proerty[index].bPropertyName[i];
                pdata++;
                *pdata = 0;
                pdata++;
            }
            *((uint32_t *)pdata) = usb_os_proerty[index].dwPropertyDataLength;
            pdata += 4;
            for(i = 0;i < usb_os_proerty[index].dwPropertyDataLength/2;i++)
            {
                *pdata = usb_os_proerty[index].bPropertyData[i];
                pdata++;
                *pdata = 0;
                pdata++;
            }
        }
    }
    usbd_ep0_write(func->device, data, setup->wLength);
}

static LIST_HEAD(class_list);

int usbd_class_register(udclass_t udclass)
{
#ifndef CONFIG_USB_DEVICE_COMPOSITE
    if(!list_empty(&class_list))
    {
        pr_info("[D/USBD] If you want to use usb composite device please define CONFIG_USB_DEVICE_COMPOSITE\r\n");
        return -1;
    }
#endif
    list_add(&udclass->list, &class_list);
    return 0;
}

int usb_device_init(udcd_t udc)
{
    udevice_t udevice;
    uconfig_t cfg;
    ufunction_t func = NULL;
    udclass_t udclass;

    if(list_empty(&class_list))
    {
        pr_info("[D/USBD] No class register on usb device\r\n");
        return -1;
    }

    /* create and startup usb device thread */
    usbd_core_init();

    /* create a device object */
    udevice = usbd_device_new();

    /* set usb controller driver to the device */
    usbd_device_set_controller(udevice, udc);

    /* create a configuration object */
    cfg = usbd_config_new();

    usbd_device_set_os_comp_id_desc(udevice, &usb_comp_id_desc);

    list_for_each_entry(udclass, &class_list, list) {
        /* create a function object */
        func = udclass->usbd_function_create(udevice);
        /* add the function to the configuration */
        usbd_config_add_function(cfg, func);
    }
    /* set device descriptor to the device */
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    pr_info("composite device config\r\n");
    usbd_device_set_descriptor(udevice, &compsit_desc);
    usbd_device_set_string(udevice, ustring);
    if(udevice->dcd->device_is_hs)
    {
        usbd_device_set_qualifier(udevice, &dev_qualifier);
    }
#else
    if (func != NULL)
        usbd_device_set_descriptor(udevice, func->dev_desc);
    else
        pr_err("%s[%d]: func is NULL\r\n", __func__, __LINE__);
#endif

    /* add the configuration to the device */
    usbd_device_add_config(udevice, cfg);

    /* initialize usb device controller */
    udc->init();

    /* set default configuration to 1 */
    usbd_set_config(udevice, 1);

    return 0;
}
