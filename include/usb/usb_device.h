/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef  __NOS_USB_DEVICE_H__
#define  __NOS_USB_DEVICE_H__

#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/device.h>
#include <string.h>
#include <usb/usb_common.h>

/* Vendor ID */
#ifdef CONFIG_USB_VENDOR_ID
#define _VENDOR_ID                  CONFIG_USB_VENDOR_ID
#else
#define _VENDOR_ID                  0x0EFF
#endif
/* Product ID */
#ifdef CONFIG_USB_PRODUCT_ID
#define _PRODUCT_ID                 CONFIG_USB_PRODUCT_ID
#else
#define _PRODUCT_ID                 0x0001
#endif

#define USB_BCD_DEVICE              0x0200   /* USB Specification Release Number in Binary-Coded Decimal */
#define USB_BCD_VERSION             0x0200   /* USB 2.0 */
#define EP0_IN_ADDR                 0x80
#define EP0_OUT_ADDR                0x00
#define EP_HANDLER(ep, func, size) \
    do { \
        /* WK_ERROR(ep != NULL); */ \
        ep->handler(func, size); \
    } while(0)
#define EP_ADDRESS(ep)              ep->ep_desc->bEndpointAddress
#define EP_MAXPACKET(ep)            ep->ep_desc->wMaxPacketSize
#define FUNC_ENABLE(func)           do{                                             \
                                        if(func->ops->enable != NULL &&          \
                                            func->enabled == false)              \
                                        {                                           \
                                            if(func->ops->enable(func) == 0)   \
                                                func->enabled = true;            \
                                        }                                           \
                                    }while(0)
#define FUNC_DISABLE(func)          do{                                             \
                                        if(func->ops->disable != NULL &&         \
                                            func->enabled == true)               \
                                        {                                           \
                                                func->enabled = false;           \
                                                func->ops->disable(func);           \
                                        }                                           \
                                    }while(0)

struct ufunction;
struct udevice;
struct uendpoint;

typedef enum
{
    /* request to read full count */
    UIO_REQUEST_READ_FULL,
    /* request to read any count */
    UIO_REQUEST_READ_BEST,
    /* request to write full count */
    UIO_REQUEST_WRITE,
}UIO_REQUEST_TYPE;

struct udcd_ops
{
    int (*set_address)(uint8_t address);
    int (*set_config)(uint8_t address);
    int (*ep_set_stall)(uint8_t address);
    int (*ep_clear_stall)(uint8_t address);
    int (*ep_enable)(struct uendpoint* ep);
    int (*ep_disable)(struct uendpoint* ep);
    size_t (*ep_read_prepare)(uint8_t address, void *buffer, size_t size);
    size_t (*ep_read)(uint8_t address, void *buffer);
    size_t (*ep_write)(uint8_t address, void *buffer, size_t size);
    int (*ep0_send_status)(void);
    int (*suspend)(void);
    int (*wakeup)(void);
};

struct ep_id
{
    uint8_t  addr;
    uint8_t  type;
    uint8_t  dir;
    uint16_t maxpacket;
    uint8_t  status;
};

typedef int (*udep_handler_t)(struct ufunction* func, size_t size);

struct uio_request
{
    struct list_head list;
    UIO_REQUEST_TYPE req_type;
    uint8_t* buffer;
    size_t size;
    size_t remain_size;
};
typedef struct uio_request* uio_request_t;

struct uendpoint
{
    struct list_head list;
    uep_desc_t ep_desc;
    struct list_head request_list;
    struct uio_request request;
    uint8_t* buffer;
    bool stalled;
    struct ep_id *id;
    udep_handler_t handler;
    int (*rx_indicate)(struct udevice* dev, size_t size);
};
typedef struct uendpoint* uep_t;

struct udcd
{
    struct device dev;
    int (*init)(void);
    const struct udcd_ops* ops;
    struct uendpoint ep0;
    uep0_stage_t stage;
    struct ep_id* ep_pool;
    uint8_t device_is_hs;
};
typedef struct udcd* udcd_t;

struct ualtsetting
{
    struct list_head list;
    uintf_desc_t intf_desc;
    void* desc;
    size_t desc_size;
    struct list_head ep_list;
};
typedef struct ualtsetting* ualtsetting_t;

typedef int (*uintf_handler_t)(struct ufunction* func, ureq_t setup);

struct uinterface
{
    struct list_head list;
    uint8_t intf_num;
    ualtsetting_t curr_setting;
    struct list_head setting_list;
    uintf_handler_t handler;
};
typedef struct uinterface* uintf_t;

struct ufunction_ops
{
    int (*enable)(struct ufunction* func);
    int (*disable)(struct ufunction* func);
    int (*sof_handler)(struct ufunction* func);
};
typedef struct ufunction_ops* ufunction_ops_t;

struct ufunction
{
    struct list_head list;
    ufunction_ops_t ops;
    struct udevice* device;
    udev_desc_t dev_desc;
    void* user_data;
    bool enabled;

    struct list_head intf_list;
};
typedef struct ufunction* ufunction_t;

struct uconfig
{
    struct list_head list;
    struct uconfig_descriptor cfg_desc;
    struct list_head func_list;
};
typedef struct uconfig* uconfig_t;

struct udevice
{
    struct list_head list;
    struct udevice_descriptor dev_desc;

    struct usb_qualifier_descriptor * dev_qualifier;
    usb_os_comp_id_desc_t    os_comp_id_desc;
    const char** str;

    udevice_state_t state;
    struct list_head cfg_list;
    uconfig_t curr_cfg;
    uint8_t nr_intf;

    udcd_t dcd;
};
typedef struct udevice* udevice_t;

struct udclass
{
    struct list_head list;
    ufunction_t (*usbd_function_create)(udevice_t device);
};
typedef struct udclass* udclass_t;

enum udev_msg_type
{
    USB_MSG_SETUP_NOTIFY,
    USB_MSG_DATA_NOTIFY,
    USB_MSG_EP0_OUT,
    USB_MSG_EP_CLEAR_FEATURE,
    USB_MSG_SOF,
    USB_MSG_RESET,
    USB_MSG_PLUG_IN,
    /* we don't need to add a "PLUG_IN" event because after the cable is
     * plugged in(before any SETUP) the classed have nothing to do. If the host
     * is ready, it will send RESET and we will have USB_MSG_RESET. So, a RESET
     * should reset and run the class while plug_in is not. */
    USB_MSG_PLUG_OUT,
};
typedef enum udev_msg_type udev_msg_type;

struct ep_msg
{
    size_t size;
    uint8_t ep_addr;
};

struct udev_msg
{
    udev_msg_type type;
    udcd_t dcd;
    union
    {
        struct ep_msg   ep_msg;
        struct urequest setup;
    } content;
};
typedef struct udev_msg* udev_msg_t;

int usbd_class_list_init(void);
udevice_t usbd_device_new(void);
uconfig_t usbd_config_new(void);
ufunction_t usbd_function_new(udevice_t device, udev_desc_t dev_desc,
                              ufunction_ops_t ops);
uintf_t usbd_interface_new(udevice_t device, uintf_handler_t handler);
uep_t usbd_endpoint_new(uep_desc_t ep_desc, udep_handler_t handler);
ualtsetting_t usbd_altsetting_new(size_t desc_size);

int usbd_core_init(void);
int usb_device_init(udcd_t udc);
int usbd_event_signal(struct udev_msg* msg);
int usbd_device_set_controller(udevice_t device, udcd_t dcd);
int usbd_device_set_descriptor(udevice_t device, udev_desc_t dev_desc);
int usbd_device_set_string(udevice_t device, const char** ustring);
int usbd_device_set_qualifier(udevice_t device, struct usb_qualifier_descriptor* qualifier);
int usbd_device_set_os_comp_id_desc(udevice_t device, usb_os_comp_id_desc_t os_comp_id_desc);
int usbd_device_add_config(udevice_t device, uconfig_t cfg);
int usbd_config_add_function(uconfig_t cfg, ufunction_t func);
int usbd_class_register(udclass_t udclass);
int usbd_function_add_interface(ufunction_t func, uintf_t intf);
int usbd_interface_add_altsetting(uintf_t intf, ualtsetting_t setting);
int usbd_altsetting_add_endpoint(ualtsetting_t setting, uep_t ep);
int usbd_os_comp_id_desc_add_os_func_comp_id_desc(usb_os_comp_id_desc_t os_comp_id_desc, usb_os_func_comp_id_desc_t os_func_comp_id_desc);
int usbd_altsetting_config_descriptor(ualtsetting_t setting, const void* desc, addr_t intf_pos);
int usbd_set_config(udevice_t device, uint8_t value);
int usbd_set_altsetting(uintf_t intf, uint8_t value);

udevice_t usbd_find_device(udcd_t dcd);
uconfig_t usbd_find_config(udevice_t device, uint8_t value);
uintf_t usbd_find_interface(udevice_t device, uint8_t value, ufunction_t *pfunc);
uep_t usbd_find_endpoint(udevice_t device, ufunction_t* pfunc, uint8_t ep_addr);
size_t usbd_io_request(udevice_t device, uep_t ep, uio_request_t req);
size_t usbd_ep0_write(udevice_t device, void *buffer, size_t size);
size_t usbd_ep0_read(udevice_t device, void *buffer, size_t size,
    int (*rx_ind)(udevice_t device, size_t size));

int usbd_vcom_class_register(void);
int usbd_ecm_class_register(void);
int usbd_hid_class_register(void);
int usbd_msc_class_register(void);
int usbd_rndis_class_register(void);
int usbd_winusb_class_register(void);

int usbd_function_set_iad(ufunction_t func, uiad_desc_t iad_desc);

int usbd_set_feature(udevice_t device, uint16_t value, uint16_t index);
int usbd_clear_feature(udevice_t device, uint16_t value, uint16_t index);
int usbd_ep_set_stall(udevice_t device, uep_t ep);
int usbd_ep_clear_stall(udevice_t device, uep_t ep);
int usbd_ep0_set_stall(udevice_t device);
int usbd_ep0_clear_stall(udevice_t device);
int usbd_ep0_setup_handler(udcd_t dcd, struct urequest* setup);
int usbd_ep0_in_handler(udcd_t dcd);
int usbd_ep0_out_handler(udcd_t dcd, size_t size);
int usbd_ep_in_handler(udcd_t dcd, uint8_t address, size_t size);
int usbd_ep_out_handler(udcd_t dcd, uint8_t address, size_t size);
int usbd_reset_handler(udcd_t dcd);
int usbd_connect_handler(udcd_t dcd);
int usbd_disconnect_handler(udcd_t dcd);
int usbd_sof_handler(udcd_t dcd);

inline int dcd_set_address(udcd_t dcd, uint8_t address)
{
    return dcd->ops->set_address(address);
}

inline int dcd_set_config(udcd_t dcd, uint8_t address)
{
    return dcd->ops->set_config(address);
}

inline int dcd_ep_enable(udcd_t dcd, uep_t ep)
{
    return dcd->ops->ep_enable(ep);
}

inline int dcd_ep_disable(udcd_t dcd, uep_t ep)
{
    return dcd->ops->ep_disable(ep);
}

inline size_t dcd_ep_read_prepare(udcd_t dcd, uint8_t address, void *buffer,
                               size_t size)
{
    if(dcd->ops->ep_read_prepare != NULL) {
        return dcd->ops->ep_read_prepare(address, buffer, size);
    } else {
        return 0;
    }
}

inline size_t dcd_ep_read(udcd_t dcd, uint8_t address, void *buffer)
{
    if(dcd->ops->ep_read != NULL) {
        return dcd->ops->ep_read(address, buffer);
    } else {
        return 0;
    }
}

inline size_t dcd_ep_write(udcd_t dcd, uint8_t address, void *buffer,
                                 size_t size)
{
    return dcd->ops->ep_write(address, buffer, size);
}

inline int dcd_ep0_send_status(udcd_t dcd)
{
    return dcd->ops->ep0_send_status();
}

inline int dcd_ep_set_stall(udcd_t dcd, uint8_t address)
{
    return dcd->ops->ep_set_stall(address);
}

inline int dcd_ep_clear_stall(udcd_t dcd, uint8_t address)
{
    return dcd->ops->ep_clear_stall(address);
}

void usbd_os_proerty_descriptor_send(ufunction_t func, ureq_t setup,
                                     usb_os_proerty_t usb_os_proerty,
                                     uint8_t number_of_proerty);

#endif /* __NOS_USB_DEVICE_H__ */
