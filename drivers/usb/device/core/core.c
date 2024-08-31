/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[USB_CORE]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/msg_queue.h>
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <usb/usb_common.h>
#include <usb/usb_device.h>

static struct list_head usb_device_list;
static struct mutex ep_write_lock;

static size_t usbd_ep_write(udevice_t device, uep_t ep, void *buffer, size_t size);
static size_t usbd_ep_read_prepare(udevice_t device, uep_t ep, void *buffer, size_t size);
static int usbd_ep_assign(udevice_t device, uep_t ep);
int usbd_ep_unassign(udevice_t device, uep_t ep);

inline unsigned int list_len(const struct list_head *l)
{
    unsigned int len = 0;
    const struct list_head *p = l;
    while (p->next != l)
    {
        p = p->next;
        len ++;
    }

    return len;
}


/**
 * This function will handle get_device_descriptor bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _get_device_descriptor(struct udevice* device, ureq_t setup)
{
    size_t size;

    pr_info("_get_device_descriptor\r\n");

    /* device descriptor wLength should less than USB_DESC_LENGTH_DEVICE*/
    size = (setup->wLength > USB_DESC_LENGTH_DEVICE) ?
           USB_DESC_LENGTH_DEVICE : setup->wLength;

    /* send device descriptor to endpoint 0 */
    usbd_ep0_write(device, (uint8_t*) &device->dev_desc, size);

    return 0;
}

/**
 * This function will handle get_config_descriptor bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _get_config_descriptor(struct udevice* device, ureq_t setup)
{
    size_t size;
    ucfg_desc_t cfg_desc;

    pr_info("_get_config_descriptor\r\n");

    cfg_desc = &device->curr_cfg->cfg_desc;
    size = (setup->wLength > cfg_desc->wTotalLength) ?
           cfg_desc->wTotalLength : setup->wLength;

    /* send configuration descriptor to endpoint 0 */
    usbd_ep0_write(device, (uint8_t*)cfg_desc, size);

    return 0;
}

/**
 * This function will handle get_string_descriptor bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful, -1 on invalid bRequest.
 */
static int _get_string_descriptor(struct udevice* device, ureq_t setup)
{
    struct ustring_descriptor str_desc;
    uint8_t index, i;
    uint32_t len;

    pr_info("_get_string_descriptor\r\n");

    str_desc.type = USB_DESC_TYPE_STRING;
    index = setup->wValue & 0xFF;

    if(index == 0xEE)
    {
        index = USB_STRING_OS_INDEX;
    }

    if(index > USB_STRING_MAX)
    {
        pr_err("unknown string index\r\n");
        usbd_ep0_set_stall(device);
        return -1;
    }
    else if(index == USB_STRING_LANGID_INDEX)
    {
        str_desc.bLength = 4;
        str_desc.String[0] = 0x09;
        str_desc.String[1] = 0x04;
    }
    else
    {
        len = strlen(device->str[index]);
        str_desc.bLength = len*2 + 2;

        for(i=0; i<len; i++)
        {
            str_desc.String[i*2] = device->str[index][i];
            str_desc.String[i*2 + 1] = 0;
        }
    }

    if (setup->wLength > str_desc.bLength)
        len = str_desc.bLength;
    else
        len = setup->wLength;

    /* send string descriptor to endpoint 0 */
    usbd_ep0_write(device, (uint8_t*)&str_desc, len);

    return 0;
}

static int _get_qualifier_descriptor(struct udevice* device, ureq_t setup)
{
    pr_info("_get_qualifier_descriptor\r\n");

    if(device->dev_qualifier && device->dcd->device_is_hs)
    {
        /* send device qualifier descriptor to endpoint 0 */
        usbd_ep0_write(device, (uint8_t*)device->dev_qualifier,
                     sizeof(struct usb_qualifier_descriptor));
    }
    else
    {
        usbd_ep0_set_stall(device);
    }

    return 0;
}

/**
 * This function will handle get_descriptor bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _get_descriptor(struct udevice* device, ureq_t setup)
{
    if(setup->request_type == USB_REQ_TYPE_DIR_IN)
    {
        switch(setup->wValue >> 8)
        {
        case USB_DESC_TYPE_DEVICE:
            _get_device_descriptor(device, setup);
            break;
        case USB_DESC_TYPE_CONFIGURATION:
            _get_config_descriptor(device, setup);
            break;
        case USB_DESC_TYPE_STRING:
            _get_string_descriptor(device, setup);
            break;
        case USB_DESC_TYPE_DEVICEQUALIFIER:
            _get_qualifier_descriptor(device, setup);
            break;
        case USB_DESC_TYPE_OTHERSPEED:
            _get_config_descriptor(device, setup);
            break;
        default:
            pr_err("unsupported descriptor request\r\n");
            usbd_ep0_set_stall(device);
            break;
        }
    }
    else
    {
        pr_err("request direction error\r\n");
        usbd_ep0_set_stall(device);
    }

    return 0;
}

/**
 * This function will handle get_interface bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _get_interface(struct udevice* device, ureq_t setup)
{
    uint8_t value;
    uintf_t intf;

    pr_info("_get_interface\r\n");

    if (device->state != USB_STATE_CONFIGURED)
    {
        usbd_ep0_set_stall(device);
        return -1;
    }

    /* find the specified interface and its alternate setting */
    intf = usbd_find_interface(device, setup->wIndex & 0xFF, NULL);
    value = intf->curr_setting->intf_desc->bAlternateSetting;

    /* send the interface alternate setting to endpoint 0*/
    usbd_ep0_write(device, &value, 1);

    return 0;
}

/**
 * This function will handle set_interface bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _set_interface(struct udevice* device, ureq_t setup)
{
    uintf_t intf;
    uep_t ep;
    struct list_head* i;
    ualtsetting_t setting;

    pr_info("_set_interface\r\n");

    if (device->state != USB_STATE_CONFIGURED)
    {
        usbd_ep0_set_stall(device);
        return -1;
    }

    /* find the specified interface */
    intf = usbd_find_interface(device, setup->wIndex & 0xFF, NULL);

    /* set alternate setting to the interface */
    usbd_set_altsetting(intf, setup->wValue & 0xFF);
    setting = intf->curr_setting;

    /* start all endpoints of the interface alternate setting */
    for(i=setting->ep_list.next; i != &setting->ep_list; i=i->next)
    {
        ep = (uep_t)list_entry(i, struct uendpoint, list);
        dcd_ep_disable(device->dcd, ep);
        dcd_ep_enable(device->dcd, ep);
    }
    dcd_ep0_send_status(device->dcd);

    return 0;
}

/**
 * This function will handle get_config bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _get_config(struct udevice* device, ureq_t setup)
{
    uint8_t value;

    pr_info("_get_config\r\n");

    if (device->state == USB_STATE_CONFIGURED)
    {
        /* get current configuration */
        value = device->curr_cfg->cfg_desc.bConfigurationValue;
    }
    else
    {
        value = 0;
    }
    /* write the current configuration to endpoint 0 */
    usbd_ep0_write(device, &value, 1);

    return 0;
}

/**
 * This function will handle set_config bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _set_config(struct udevice* device, ureq_t setup)
{
    struct list_head *i, *j, *k;
    uconfig_t cfg;
    uintf_t intf;
    ualtsetting_t setting;
    uep_t ep;

    pr_info("_set_config\r\n");

    if (setup->wValue > device->dev_desc.bNumConfigurations)
    {
        usbd_ep0_set_stall(device);
        return -1;
    }

    if (setup->wValue == 0)
    {
        pr_info("address state\r\n");
        device->state = USB_STATE_ADDRESS;

        goto _exit;
    }

    /* set current configuration */
    usbd_set_config(device, setup->wValue);
    cfg = device->curr_cfg;

    for (i=cfg->func_list.next; i!=&cfg->func_list; i=i->next)
    {
        /* run all functiones and their endpoints in the configuration */
        ufunction_t func = (ufunction_t)list_entry(i, struct ufunction, list);
        for(j=func->intf_list.next; j!=&func->intf_list; j=j->next)
        {
            intf = (uintf_t)list_entry(j, struct uinterface, list);
            setting = intf->curr_setting;
            for(k=setting->ep_list.next; k != &setting->ep_list; k=k->next)
            {
                ep = (uep_t)list_entry(k, struct uendpoint, list);

                /* first disable then enable an endpoint */
                dcd_ep_disable(device->dcd, ep);
                dcd_ep_enable(device->dcd, ep);
            }
        }
        /* after enabled endpoints, then enable function */
        FUNC_ENABLE(func);
    }

    device->state = USB_STATE_CONFIGURED;

_exit:
    /* issue status stage */
    dcd_ep0_send_status(device->dcd);

    return 0;
}

/**
 * This function will handle set_address bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _set_address(struct udevice* device, ureq_t setup)
{
    /* set address in device control driver */
    dcd_set_address(device->dcd, setup->wValue);

    /* issue status stage */
    dcd_ep0_send_status(device->dcd);

    pr_info("_set_address\r\n");

    device->state = USB_STATE_ADDRESS;

    return 0;
}

/**
 * This function will handle standard bRequest to
 * interface that defined in function-specifics
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _request_interface(struct udevice* device, ureq_t setup)
{
    uintf_t intf;
    ufunction_t func;
    int ret;

    pr_info("_request_interface\r\n");

    intf = usbd_find_interface(device, setup->wIndex & 0xFF, &func);
    if (intf != NULL)
        ret = intf->handler(func, setup);
    else
        ret = -1;

    return ret;
}

/**
 * This function will handle standard bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful.
 */
static int _standard_request(struct udevice* device, ureq_t setup)
{
    udcd_t dcd;
    uint16_t value = 0;

    dcd = device->dcd;

    switch(setup->request_type & USB_REQ_TYPE_RECIPIENT_MASK)
    {
    case USB_REQ_TYPE_DEVICE:
        switch(setup->bRequest)
        {
        case USB_REQ_GET_STATUS:
            usbd_ep0_write(device, &value, 2);
            break;
        case USB_REQ_CLEAR_FEATURE:
            usbd_clear_feature(device, setup->wValue, setup->wIndex);
            dcd_ep0_send_status(dcd);
            break;
        case USB_REQ_SET_FEATURE:
            usbd_set_feature(device, setup->wValue, setup->wIndex);
            break;
        case USB_REQ_SET_ADDRESS:
            _set_address(device, setup);
            break;
        case USB_REQ_GET_DESCRIPTOR:
            _get_descriptor(device, setup);
            break;
        case USB_REQ_SET_DESCRIPTOR:
            usbd_ep0_set_stall(device);
            break;
        case USB_REQ_GET_CONFIGURATION:
            _get_config(device, setup);
            break;
        case USB_REQ_SET_CONFIGURATION:
            _set_config(device, setup);
            break;
        default:
            pr_err("unknown device request\r\n");
            usbd_ep0_set_stall(device);
            break;
        }
        break;
    case USB_REQ_TYPE_INTERFACE:
        switch(setup->bRequest)
        {
        case USB_REQ_GET_INTERFACE:
            _get_interface(device, setup);
            break;
        case USB_REQ_SET_INTERFACE:
            _set_interface(device, setup);
            break;
        default:
            if (_request_interface(device, setup) != 0)
            {
                pr_err("unknown interface request\r\n");
                usbd_ep0_set_stall(device);
                return - 1;
            }
            else
                break;
        }
        break;
    case USB_REQ_TYPE_ENDPOINT:
        switch(setup->bRequest)
        {
        case USB_REQ_GET_STATUS:
        {
            uep_t ep;

            ep = usbd_find_endpoint(device, NULL, setup->wIndex);
            value = ep->stalled;
            usbd_ep0_write(device, &value, 2);
        }
        break;
        case USB_REQ_CLEAR_FEATURE:
        {
            uep_t ep;
            uio_request_t req;
            struct list_head *node;

            ep = usbd_find_endpoint(device, NULL, setup->wIndex);
            if(USB_EP_HALT == setup->wValue && ep->stalled == true)
            {
                usbd_clear_feature(device, setup->wValue, setup->wIndex);
                dcd_ep0_send_status(dcd);
                ep->stalled = false;

                for (node = ep->request_list.next; node != &ep->request_list; node = node->next)
                {
                    req = (uio_request_t)list_entry(node, struct uio_request, list);
                    usbd_io_request(device, ep, req);
                    pr_info("fired a request\r\n");
                }

                INIT_LIST_HEAD(&ep->request_list);
            }
        }
        break;
        case USB_REQ_SET_FEATURE:
        {
            uep_t ep;

            if(USB_EP_HALT == setup->wValue)
            {
                ep = usbd_find_endpoint(device, NULL, setup->wIndex);
                ep->stalled = true;
                usbd_set_feature(device, setup->wValue, setup->wIndex);
                dcd_ep0_send_status(dcd);
            }
        }
        break;
        case USB_REQ_SYNCH_FRAME:
            break;
        default:
            pr_err("unknown endpoint request\r\n");
            usbd_ep0_set_stall(device);
            break;
        }
        break;
    case USB_REQ_TYPE_OTHER:
        pr_err("unknown other type request\r\n");
        usbd_ep0_set_stall(device);
        break;
    default:
        pr_err("unknown type request\r\n");
        usbd_ep0_set_stall(device);
        break;
    }

    return 0;
}

/**
 * This function will handle function bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful, -1 on invalid bRequest.
 */
static int _function_request(udevice_t device, ureq_t setup)
{
    uintf_t intf;
    ufunction_t func;

    /* verify bRequest wValue */
    if(setup->wIndex > device->curr_cfg->cfg_desc.bNumInterfaces)
    {
        usbd_ep0_set_stall(device);
        return -1;
    }

    switch(setup->request_type & USB_REQ_TYPE_RECIPIENT_MASK)
    {
    case USB_REQ_TYPE_INTERFACE:
        intf = usbd_find_interface(device, setup->wIndex & 0xFF, &func);
        if(intf == NULL)
        {
            pr_err("unkwown interface request\r\n");
            usbd_ep0_set_stall(device);
        }
        else
        {
            intf->handler(func, setup);
        }
        break;
    case USB_REQ_TYPE_ENDPOINT:
        break;
    default:
        pr_err("unknown function request type\r\n");
        usbd_ep0_set_stall(device);
        break;
    }

    return 0;
}
static int _vendor_request(udevice_t device, ureq_t setup)
{
    static uint8_t * usb_comp_id_desc = NULL;
    static uint32_t  usb_comp_id_desc_size = 0;
    usb_os_func_comp_id_desc_t func_comp_id_desc;
    uintf_t intf;
    ufunction_t func;
    switch(setup->bRequest)
    {
        case 'A':
        switch(setup->wIndex)
        {
            case 0x04:
                if(list_len(&device->os_comp_id_desc->func_desc) == 0)
                {
                    usbd_ep0_set_stall(device);
                    return 0;
                }
                if(usb_comp_id_desc == NULL)
                {
                    uint8_t * pusb_comp_id_desc;
                    struct list_head *p;
                    usb_comp_id_desc_size = sizeof(struct usb_os_header_comp_id_descriptor) +
                    (sizeof(struct usb_os_function_comp_id_descriptor)-sizeof(struct list_head))*list_len(&device->os_comp_id_desc->func_desc);

                    usb_comp_id_desc = (uint8_t *)kmalloc(usb_comp_id_desc_size, GFP_KERNEL);
                    if (usb_comp_id_desc == NULL) {
                        pr_err("alloc usb_comp_id_desc buf error\r\n");
                        return 0;
                    }
                    device->os_comp_id_desc->head_desc.dwLength = usb_comp_id_desc_size;
                    pusb_comp_id_desc = usb_comp_id_desc;
                    memcpy((void *)pusb_comp_id_desc,(void *)&device->os_comp_id_desc->head_desc,sizeof(struct usb_os_header_comp_id_descriptor));
                    pusb_comp_id_desc += sizeof(struct usb_os_header_comp_id_descriptor);

                    for (p = device->os_comp_id_desc->func_desc.next; p != &device->os_comp_id_desc->func_desc; p = p->next)
                    {
                        func_comp_id_desc = list_entry(p,struct usb_os_function_comp_id_descriptor,list);
                        memcpy(pusb_comp_id_desc,(void *)&func_comp_id_desc->bFirstInterfaceNumber,
                        sizeof(struct usb_os_function_comp_id_descriptor)-sizeof(struct list_head));
                        pusb_comp_id_desc += sizeof(struct usb_os_function_comp_id_descriptor)-sizeof(struct list_head);
                    }
                }
                usbd_ep0_write(device, (void*)usb_comp_id_desc, setup->wLength);
            break;
            case 0x05:
                intf = usbd_find_interface(device, setup->wValue & 0xFF, &func);
                if(intf != NULL)
                {
                    intf->handler(func, setup);
                }
                break;
        }

        break;
    }
    return 0;
}

static __maybe_unused int _dump_setup_packet(ureq_t setup)
{
    pr_info("[\r\n");
    pr_info("  setup_request : 0x%x\r\n",
                                setup->request_type);
    pr_info("  value         : 0x%x\r\n", setup->wValue);
    pr_info("  length        : 0x%x\r\n", setup->wLength);
    pr_info("  index         : 0x%x\r\n", setup->wIndex);
    pr_info("  request       : 0x%x\r\n", setup->bRequest);
    pr_info("]\r\n");

    return 0;
}

/**
 * This function will handle setup bRequest.
 *
 * @param device the usb device object.
 * @param setup the setup bRequest.
 *
 * @return 0 on successful, -1 on invalid bRequest.
 */
static int _setup_request(udevice_t device, ureq_t setup)
{
    //_dump_setup_packet(setup);

    switch((setup->request_type & USB_REQ_TYPE_MASK))
    {
    case USB_REQ_TYPE_STANDARD:
        _standard_request(device, setup);
        break;
    case USB_REQ_TYPE_CLASS:
        _function_request(device, setup);
        break;
    case USB_REQ_TYPE_VENDOR:
        _vendor_request(device, setup);
        break;
    default:
        pr_err("unknown setup request type\r\n");
        usbd_ep0_set_stall(device);
        return -1;
    }

    return 0;
}

/**
 * This function will hanle data notify event.
 *
 * @param device the usb device object.
 * @param ep_msg the endpoint message.
 *
 * @return 0.
 */
static int _data_notify(udevice_t device, struct ep_msg* ep_msg)
{
    uep_t ep;
    ufunction_t func;
    size_t size = 0;

    if (device->state != USB_STATE_CONFIGURED)
        return -1;

    ep = usbd_find_endpoint(device, &func, ep_msg->ep_addr);
    if (ep == NULL) {
        pr_err("invalid endpoint\r\n");
        return -1;
    }

    if (EP_ADDRESS(ep) & USB_DIR_IN) {
        size = ep_msg->size;
        if(ep->request.remain_size >= EP_MAXPACKET(ep)) {
            dcd_ep_write(device->dcd, EP_ADDRESS(ep), ep->request.buffer, EP_MAXPACKET(ep));
            ep->request.remain_size -= EP_MAXPACKET(ep);
            ep->request.buffer += EP_MAXPACKET(ep);
        } else if(ep->request.remain_size > 0) {
            dcd_ep_write(device->dcd, EP_ADDRESS(ep), ep->request.buffer, ep->request.remain_size);
            ep->request.remain_size = 0;
        } else {
            EP_HANDLER(ep, func, size);
        }
    } else {
        size = ep_msg->size;
        if(ep->request.remain_size == 0)
            return 0;
        if(size == 0)
            size = dcd_ep_read(device->dcd, EP_ADDRESS(ep), ep->request.buffer);
        ep->request.remain_size -= size;
        ep->request.buffer += size;

        if (ep->request.req_type == UIO_REQUEST_READ_BEST)
            EP_HANDLER(ep, func, size);
        else if (ep->request.remain_size == 0)
            EP_HANDLER(ep, func, ep->request.size);
        else
            dcd_ep_read_prepare(device->dcd, EP_ADDRESS(ep), ep->request.buffer,
                min(ep->request.remain_size, EP_MAXPACKET(ep)));
    }

    return 0;
}

static int _ep0_out_notify(udevice_t device, struct ep_msg* ep_msg)
{
    uep_t ep0;
    size_t size;

    ep0 = &device->dcd->ep0;
    size = ep_msg->size;

    if(ep0->request.remain_size == 0)
    {
        return 0;
    }
    if(size == 0)
    {
        size = dcd_ep_read(device->dcd, EP0_OUT_ADDR, ep0->request.buffer);
        if(size == 0)
        {
            return 0;
        }
    }

    ep0->request.remain_size -= size;
    ep0->request.buffer += size;
    if(ep0->request.remain_size == 0)
    {
        /* invoke callback */
        if(ep0->rx_indicate != NULL)
        {
            ep0->rx_indicate(device, size);
        }
    }
    else
    {
        usbd_ep0_read(device, ep0->request.buffer, ep0->request.remain_size,ep0->rx_indicate);
    }

    return 0;
}

/**
 * This function will notity sof event to all of function.
 *
 * @param device the usb device object.
 *
 * @return 0.
 */
static int _sof_notify(udevice_t device)
{
    struct list_head *i;
    ufunction_t func;

    /* to notity every function that sof event comes */
    for (i=device->curr_cfg->func_list.next;
            i!=&device->curr_cfg->func_list; i=i->next)
    {
        func = (ufunction_t)list_entry(i, struct ufunction, list);
        if(func->ops->sof_handler != NULL)
            func->ops->sof_handler(func);
    }

    return 0;
}

/**
 * This function will disable all USB functions.
 *
 * @param device the usb device object.
 *
 * @return 0.
 */
static int _stop_notify(udevice_t device)
{
    struct list_head *i;
    ufunction_t func;

    /* to notity every function */
    for (i  = device->curr_cfg->func_list.next;
         i != &device->curr_cfg->func_list;
         i  = i->next)
    {
        func = (ufunction_t)list_entry(i, struct ufunction, list);
        FUNC_DISABLE(func);
    }

    return 0;
}

static size_t usbd_ep_write(udevice_t device, uep_t ep, __maybe_unused void *buffer, size_t size)
{
    uint16_t maxpacket;

    mutex_lock(&ep_write_lock);
    maxpacket = EP_MAXPACKET(ep);
    if(ep->request.remain_size >= maxpacket) {
        dcd_ep_write(device->dcd, EP_ADDRESS(ep), ep->request.buffer, maxpacket);
        ep->request.remain_size -= maxpacket;
        ep->request.buffer += maxpacket;
    } else {
        dcd_ep_write(device->dcd, EP_ADDRESS(ep), ep->request.buffer,
            ep->request.remain_size);
        ep->request.remain_size = 0;
    }
    mutex_unlock(&ep_write_lock);
    return size;
}

static size_t usbd_ep_read_prepare(udevice_t device, uep_t ep, void *buffer, size_t size)
{
    return dcd_ep_read_prepare(device->dcd, EP_ADDRESS(ep), buffer, size > EP_MAXPACKET(ep) ? EP_MAXPACKET(ep) : size);
}

/**
 * This function will create an usb device object.
 *
 * @param ustring the usb string array to contain string descriptor.
 *
 * @return an usb device object on success, NULL on fail.
 */
udevice_t usbd_device_new(void)
{
    udevice_t udevice;

    pr_info("usbd_device_new\r\n");

    /* allocate memory for the object */
    udevice = kmalloc(sizeof(struct udevice), GFP_KERNEL);
    if (udevice == NULL) {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    memset(udevice, 0, sizeof(struct udevice));

    /* to initialize configuration list */
    INIT_LIST_HEAD(&udevice->cfg_list);

    /* insert the device object to device list */
    list_add(&udevice->list, &usb_device_list);

    return udevice;
}

/**
 * This function will set usb device string description.
 *
 * @param device the usb device object.
 * @param ustring pointer to string pointer array.
 *
 * @return 0.
 */
int usbd_device_set_string(udevice_t device, const char** ustring)
{
    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(ustring != NULL);

    /* set string descriptor array to the device object */
    device->str = ustring;

    return 0;
}

int usbd_device_set_os_comp_id_desc(udevice_t device, usb_os_comp_id_desc_t os_comp_id_desc)
{
    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(os_comp_id_desc != NULL);

    /* set string descriptor array to the device object */
    device->os_comp_id_desc = os_comp_id_desc;
    INIT_LIST_HEAD(&device->os_comp_id_desc->func_desc);
    return 0;
}

int usbd_device_set_qualifier(udevice_t device, struct usb_qualifier_descriptor* qualifier)
{
    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(qualifier != NULL);

    device->dev_qualifier = qualifier;

    return 0;
}

/**
 * This function will set an usb controller driver to a device.
 *
 * @param device the usb device object.
 * @param dcd the usb device controller driver.
 *
 * @return 0 on successful.
 */
int usbd_device_set_controller(udevice_t device, udcd_t dcd)
{
    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(dcd != NULL);

    /* set usb device controller driver to the device */
    device->dcd = dcd;

    return 0;
}

/**
 * This function will set an usb device descriptor to a device.
 *
 * @param device the usb device object.
 * @param dev_desc the usb device descriptor.
 *
 * @return 0 on successful.
 */
int usbd_device_set_descriptor(udevice_t device, udev_desc_t dev_desc)
{
    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(dev_desc != NULL);

    /* copy the usb device descriptor to the device */
    memcpy((void *)&device->dev_desc, (void *)dev_desc, USB_DESC_LENGTH_DEVICE);

    return 0;
}

/**
 * This function will create an usb configuration object.
 *
 * @param none.
 *
 * @return an usb configuration object.
 */
uconfig_t usbd_config_new(void)
{
    uconfig_t cfg;

    pr_info("usbd_config_new\r\n");

    /* allocate memory for the object */
    cfg = kmalloc(sizeof(struct uconfig), GFP_KERNEL);
    if (cfg == NULL) {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    memset(cfg, 0, sizeof(struct uconfig));

    /* set default wValue */
    cfg->cfg_desc.bLength = USB_DESC_LENGTH_CONFIG;
    cfg->cfg_desc.type = USB_DESC_TYPE_CONFIGURATION;
    cfg->cfg_desc.wTotalLength = USB_DESC_LENGTH_CONFIG;
    cfg->cfg_desc.bmAttributes = 0xC0;
    cfg->cfg_desc.MaxPower = 0x32;

    /* to initialize function object list */
    INIT_LIST_HEAD(&cfg->func_list);

    return cfg;
}

/**
 * This function will create an usb interface object.
 *
 * @param device the usb device object.
 * @handler the callback handler of object
 *
 * @return an usb interface object on success, NULL on fail.
 */
uintf_t usbd_interface_new(udevice_t device, uintf_handler_t handler)
{
    uintf_t intf;

    pr_info("usbd_interface_new\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);

    /* allocate memory for the object */
    intf = (uintf_t)kmalloc(sizeof(struct uinterface), GFP_KERNEL);
    if (intf == NULL) {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    intf->intf_num = device->nr_intf;
    device->nr_intf++;
    intf->handler = handler;
    intf->curr_setting = NULL;

    /* to initialize the alternate setting object list */
    INIT_LIST_HEAD(&intf->setting_list);

    return intf;
}

/**
 * This function will create an usb alternate setting object.
 *
 * @param intf_desc the interface descriptor.
 * @desc_size the size of the interface descriptor.
 *
 * @return an usb alternate setting object on success, NULL on fail.
 */
ualtsetting_t usbd_altsetting_new(size_t desc_size)
{
    ualtsetting_t setting;

    pr_info("usbd_altsetting_new\r\n");

    /* parameter check */
    // WK_ERROR(desc_size > 0);

    /* allocate memory for the object */
    setting = (ualtsetting_t)kmalloc(sizeof(struct ualtsetting), GFP_KERNEL);
    if(setting == NULL)
    {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    /* allocate memory for the desc */
    setting->desc = kmalloc(desc_size, GFP_KERNEL);
    if (setting->desc == NULL)
    {
        pr_err("alloc desc memery failed\r\n");
        kfree(setting);
        return NULL;
    }

    setting->desc_size = desc_size;
    setting->intf_desc = NULL;

    /* to initialize endpoint list */
    INIT_LIST_HEAD(&setting->ep_list);

    return setting;
}

/**
 * This function will config an desc in alternate setting object.
 *
 * @param setting the altsetting to be config.
 * @param desc use it to init desc in setting.
 * @param intf_pos the offset of interface descriptor in desc.
 *
 * @return 0.
 */
int usbd_altsetting_config_descriptor(ualtsetting_t setting, const void* desc, addr_t intf_pos)
{
    // WK_ERROR(setting != NULL);
    // WK_ERROR(setting->desc !=NULL);

    memcpy(setting->desc, desc, setting->desc_size);
    setting->intf_desc = (uintf_desc_t)((char*)setting->desc + intf_pos);

    return 0;
}

/**
 * This function will create an usb function object.
 *
 * @param device the usb device object.
 * @param dev_desc the device descriptor.
 * @param ops the operation set.
 *
 * @return an usb function object on success, NULL on fail.
 */
ufunction_t usbd_function_new(udevice_t device, udev_desc_t dev_desc,
                              ufunction_ops_t ops)
{
    ufunction_t func;

    pr_info("usbd_function_new\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(dev_desc != NULL);

    /* allocate memory for the object */
    func = (ufunction_t)kmalloc(sizeof(struct ufunction), GFP_KERNEL);
    if(func == NULL)
    {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    func->dev_desc = dev_desc;
    func->ops = ops;
    func->device = device;
    func->enabled = false;

    /* to initialize interface list */
    INIT_LIST_HEAD(&func->intf_list);

    return func;
}

/**
 * This function will create an usb endpoint object.
 *
 * @param ep_desc the endpoint descriptor.
 * @handler the callback handler of object
 *
 * @return an usb endpoint object on success, NULL on fail.
 */
uep_t usbd_endpoint_new(uep_desc_t ep_desc, udep_handler_t handler)
{
    uep_t ep;

    pr_info("usbd_endpoint_new\r\n");

    /* parameter check */
    // WK_ERROR(ep_desc != NULL);

    /* allocate memory for the object */
    ep = (uep_t)kmalloc(sizeof(struct uendpoint), GFP_KERNEL);
    if(ep == NULL)
    {
        pr_err("alloc memery failed\r\n");
        return NULL;
    }
    ep->ep_desc = ep_desc;
    ep->handler = handler;
    ep->buffer  = NULL;
    ep->stalled = false;
    INIT_LIST_HEAD(&ep->request_list);

    return ep;
}

/**
 * This function will find an usb device object.
 *
 * @dcd usd device controller driver.
 *
 * @return an usb device object on found or NULL on not found.
 */
udevice_t usbd_find_device(udcd_t dcd)
{
    struct list_head* node;
    udevice_t device;

    /* parameter check */
    // WK_ERROR(dcd != NULL);

    /* search a device in the the device list */
    for (node = usb_device_list.next; node != &usb_device_list; node = node->next)
    {
        device = (udevice_t)list_entry(node, struct udevice, list);
        if(device->dcd == dcd) return device;
    }

    pr_err("can't find device\r\n");
    return NULL;
}

/**
 * This function will find an usb configuration object.
 *
 * @param device the usb device object.
 * @param wValue the configuration number.
 *
 * @return an usb configuration object on found or NULL on not found.
 */
uconfig_t usbd_find_config(udevice_t device, uint8_t value)
{
    struct list_head* node;
    uconfig_t cfg = NULL;

    pr_info("usbd_find_config\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(value <= device->dev_desc.bNumConfigurations);

    /* search a configration in the the device */
    for (node = device->cfg_list.next; node != &device->cfg_list; node = node->next)
    {
        cfg = (uconfig_t)list_entry(node, struct udevice, list);
        if(cfg->cfg_desc.bConfigurationValue == value)
        {
            return cfg;
        }
    }

    pr_err("can't find configuration %d\r\n", value);
    return NULL;
}

/**
 * This function will find an usb interface object.
 *
 * @param device the usb device object.
 * @param wValue the interface number.
 *
 * @return an usb configuration object on found or NULL on not found.
 */
uintf_t usbd_find_interface(udevice_t device, uint8_t value, ufunction_t *pfunc)
{
    ufunction_t func;
    uintf_t intf;

    pr_info("usbd_find_interface\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(value < device->nr_intf);

    /* search an interface in the current configuration */
    list_for_each_entry(func, &device->curr_cfg->func_list, list) {
        list_for_each_entry(intf, &func->intf_list, list) {
            if(intf->intf_num == value) {
                if (pfunc != NULL)
                    *pfunc = func;
                return intf;
            }
        }
    }

    pr_err("can't find interface %d\r\n", value);
    return NULL;
}

/**
 * This function will find an usb interface alternate setting object.
 *
 * @param device the usb device object.
 * @param wValue the alternate setting number.
 *
 * @return an usb interface alternate setting object on found or NULL on not found.
 */
ualtsetting_t usbd_find_altsetting(uintf_t intf, uint8_t value)
{
    struct list_head *i;
    ualtsetting_t setting;

    pr_info("usbd_find_altsetting\r\n");

    /* parameter check */
    // WK_ERROR(intf != NULL);

    if(intf->curr_setting != NULL)
    {
        /* if the wValue equal to the current alternate setting, then do not search */
        if(intf->curr_setting->intf_desc->bAlternateSetting == value)
            return intf->curr_setting;
    }

    /* search a setting in the alternate setting list */
    for(i=intf->setting_list.next; i!=&intf->setting_list; i=i->next)
    {
        setting =(ualtsetting_t)list_entry(i, struct ualtsetting, list);
        if(setting->intf_desc->bAlternateSetting == value)
            return setting;
    }

    pr_err("can't find alternate setting %d\r\n", value);
    return NULL;
}

/**
 * This function will find an usb endpoint object.
 *
 * @param device the usb device object.
 * @param ep_addr endpoint address.
 *
 * @return an usb endpoint object on found or NULL on not found.
 */
uep_t usbd_find_endpoint(udevice_t device, ufunction_t* pfunc, uint8_t ep_addr)
{
    uep_t ep;
    struct list_head *i, *j, *k;
    ufunction_t func;
    uintf_t intf;

    /* parameter check */
    // WK_ERROR(device != NULL);

    /* search a endpoint in the current configuration */
    for (i=device->curr_cfg->func_list.next; i!=&device->curr_cfg->func_list; i=i->next)
    {
        func = (ufunction_t)list_entry(i, struct ufunction, list);
        for(j=func->intf_list.next; j!=&func->intf_list; j=j->next)
        {
            intf = (uintf_t)list_entry(j, struct uinterface, list);
            for(k=intf->curr_setting->ep_list.next;
                    k!=&intf->curr_setting->ep_list; k=k->next)
            {
                ep = (uep_t)list_entry(k, struct uendpoint, list);
                if(EP_ADDRESS(ep) == ep_addr)
                {
                    if (pfunc != NULL)
                        *pfunc = func;
                    return ep;
                }
            }
        }
    }

    pr_err("can't find endpoint 0x%x\r\n", ep_addr);
    return NULL;
}

/**
 * This function will add a configuration to an usb device.
 *
 * @param device the usb device object.
 * @param cfg the configuration object.
 *
 * @return 0.
 */
int usbd_device_add_config(udevice_t device, uconfig_t cfg)
{
    struct list_head *i, *j, *k;
    ufunction_t func;
    uintf_t intf;
    uep_t ep;

    pr_info("usbd_device_add_config\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(cfg != NULL);

    /* set configuration number to the configuration descriptor */
    cfg->cfg_desc.bConfigurationValue = device->dev_desc.bNumConfigurations + 1;
    device->dev_desc.bNumConfigurations++;

    for (i=cfg->func_list.next; i!=&cfg->func_list; i=i->next)
    {
        func = (ufunction_t)list_entry(i, struct ufunction, list);

        for(j=func->intf_list.next; j!=&func->intf_list; j=j->next)
        {
            intf = (uintf_t)list_entry(j, struct uinterface, list);
            cfg->cfg_desc.bNumInterfaces++;

            /* allocate address for every endpoint in the interface alternate setting */
            for(k=intf->curr_setting->ep_list.next;
                    k!=&intf->curr_setting->ep_list; k=k->next)
            {
                ep = (uep_t)list_entry(k, struct uendpoint, list);
                if(usbd_ep_assign(device, ep) != 0)
                {
                    pr_err("endpoint assign error\r\n");
                }
            }

            /* construct complete configuration descriptor */
            memcpy((void*)&cfg->cfg_desc.data[cfg->cfg_desc.wTotalLength - USB_DESC_LENGTH_CONFIG],
                        (void*)intf->curr_setting->desc,
                        intf->curr_setting->desc_size);
            cfg->cfg_desc.wTotalLength += intf->curr_setting->desc_size;
        }
    }

    /* insert the configuration to the list */
    list_add(&cfg->list, &device->cfg_list);

    return 0;
}

/**
 * This function will add a function to a configuration.
 *
 * @param cfg the configuration object.
 * @param func the function object.
 *
 * @return 0.
 */
int usbd_config_add_function(uconfig_t cfg, ufunction_t func)
{
    pr_info("usbd_config_add_function\r\n");

    /* parameter check */
    // WK_ERROR(cfg != NULL);
    // WK_ERROR(func != NULL);

    /* insert the function to the list */
    list_add(&func->list, &cfg->func_list);

    return 0;
}

/**
 * This function will add an interface to a function.
 *
 * @param func the function object.
 * @param intf the interface object.
 *
 * @return 0.
 */
int usbd_function_add_interface(ufunction_t func, uintf_t intf)
{

    pr_info("usbd_function_add_interface\r\n");

    /* parameter check */
    // WK_ERROR(func != NULL);
    // WK_ERROR(intf != NULL);

    /* insert the interface to the list */
    list_add(&intf->list, &func->intf_list);

    return 0;
}

/**
 * This function will add an alternate setting to an interface.
 *
 * @param intf the interface object.
 * @param setting the alternate setting object.
 *
 * @return 0.
 */
int usbd_interface_add_altsetting(uintf_t intf, ualtsetting_t setting)
{
    pr_info("usbd_interface_add_altsetting\r\n");

    /* parameter check */
    // WK_ERROR(intf != NULL);
    // WK_ERROR(setting != NULL);

    setting->intf_desc->bInterfaceNumber = intf->intf_num;

    /* insert the alternate setting to the list */
    list_add(&setting->list, &intf->setting_list);

    return 0;
}

/**
 * This function will add an endpoint to an alternate setting.
 *
 * @param setting the alternate setting object.
 * @param ep the endpoint object.
 *
 * @return 0.
 */
int usbd_altsetting_add_endpoint(ualtsetting_t setting, uep_t ep)
{
    pr_info("usbd_altsetting_add_endpoint\r\n");

    /* parameter check */
    // WK_ERROR(setting != NULL);
    // WK_ERROR(ep != NULL);

    /* insert the endpoint to the list */
    list_add(&ep->list, &setting->ep_list);

    return 0;
}

int usbd_os_comp_id_desc_add_os_func_comp_id_desc(usb_os_comp_id_desc_t os_comp_id_desc, usb_os_func_comp_id_desc_t os_func_comp_id_desc)
{
    // WK_ERROR(os_comp_id_desc != NULL);
    // WK_ERROR(os_func_comp_id_desc != NULL);
    list_add(&os_func_comp_id_desc->list, &os_comp_id_desc->func_desc);
    os_comp_id_desc->head_desc.bCount++;
    return 0;
}

/**
 * This function will set an alternate setting for an interface.
 *
 * @param intf_desc the interface descriptor.
 * @param wValue the alternate setting number.
 *
 * @return 0.
 */
int usbd_set_altsetting(uintf_t intf, uint8_t value)
{
    ualtsetting_t setting;

    pr_info("usbd_set_altsetting\r\n");

    /* parameter check */
    // WK_ERROR(intf != NULL);

    /* find an alternate setting */
    setting = usbd_find_altsetting(intf, value);

    /* set as current alternate setting */
    intf->curr_setting = setting;

    return 0;
}

/**
 * This function will set a configuration for an usb device.
 *
 * @param device the usb device object.
 * @param wValue the configuration number.
 *
 * @return 0.
 */
int usbd_set_config(udevice_t device, uint8_t value)
{
    uconfig_t cfg;

    pr_info("usbd_set_config\r\n");

    /* parameter check */
    // WK_ERROR(device != NULL);
    // WK_ERROR(value <= device->dev_desc.bNumConfigurations);

    /* find a configuration */
    cfg = usbd_find_config(device, value);

    /* set as current configuration */
    device->curr_cfg = cfg;

    dcd_set_config(device->dcd, value);

    return true;
}

/**
 * This function will bRequest an IO transaction.
 *
 * @param device the usb device object.
 * @param ep the endpoint object.
 * @param req IO bRequest.
 *
 * @return 0.
 */
size_t usbd_io_request(udevice_t device, uep_t ep, uio_request_t req)
{
    size_t size = 0;

    // WK_ERROR(device != NULL);
    // WK_ERROR(req != NULL);

    if (ep->stalled == false) {
        switch (req->req_type) {
        case UIO_REQUEST_READ_BEST:
        case UIO_REQUEST_READ_FULL:
            ep->request.remain_size = ep->request.size;
            size = usbd_ep_read_prepare(device, ep, req->buffer, req->size);
            break;
        case UIO_REQUEST_WRITE:
            ep->request.remain_size = ep->request.size;
            size = usbd_ep_write(device, ep, req->buffer, req->size);
            break;
        default:
            pr_err("unknown request type\r\n");
            break;
        }
    } else {
        list_add(&req->list, &ep->request_list);
        pr_info("suspend a request\r\n");
    }

    return size;
}

/**
 * This function will set feature for an usb device.
 *
 * @param device the usb device object.
 * @param wValue the configuration number.
 *
 * @return 0.
 */
int usbd_set_feature(udevice_t device, uint16_t value, uint16_t index)
{
    // WK_ERROR(device != NULL);

    if (value == USB_FEATURE_DEV_REMOTE_WAKEUP) {
        pr_info("set feature remote wakeup\r\n");
    } else if (value == USB_FEATURE_ENDPOINT_HALT) {
        pr_info("set feature stall\r\n");
        dcd_ep_set_stall(device->dcd, (uint32_t)(index & 0xFF));
    }

    return 0;
}

/**
 * This function will clear feature for an usb device.
 *
 * @param device the usb device object.
 * @param wValue the configuration number.
 *
 * @return 0.
 */
int usbd_clear_feature(udevice_t device, uint16_t value, uint16_t index)
{
    // WK_ERROR(device != NULL);

    if (value == USB_FEATURE_DEV_REMOTE_WAKEUP) {
        pr_info("clear feature remote wakeup\r\n");
    } else if (value == USB_FEATURE_ENDPOINT_HALT) {
        pr_info("clear feature stall\r\n");
        dcd_ep_clear_stall(device->dcd, (uint32_t)(index & 0xFF));
    }

    return 0;
}

int usbd_ep0_set_stall(udevice_t device)
{
    // WK_ERROR(device != NULL);

    return dcd_ep_set_stall(device->dcd, 0);
}

int usbd_ep0_clear_stall(udevice_t device)
{
    // WK_ERROR(device != NULL);

    return dcd_ep_clear_stall(device->dcd, 0);
}

int usbd_ep_set_stall(udevice_t device, uep_t ep)
{
    int ret;

    // WK_ERROR(device != NULL);
    // WK_ERROR(ep != NULL);
    // WK_ERROR(ep->ep_desc != NULL);

    ret = dcd_ep_set_stall(device->dcd, EP_ADDRESS(ep));
    if(ret == 0)
        ep->stalled = true;

    return ret;
}

int usbd_ep_clear_stall(udevice_t device, uep_t ep)
{
    int ret;

    // WK_ERROR(device != NULL);
    // WK_ERROR(ep != NULL);
    // WK_ERROR(ep->ep_desc != NULL);

    ret = dcd_ep_clear_stall(device->dcd, EP_ADDRESS(ep));
    if(ret == 0)
        ep->stalled = false;

    return ret;
}

static int usbd_ep_assign(udevice_t device, uep_t ep)
{
    int i = 0;

    // WK_ERROR(device != NULL);
    // WK_ERROR(device->dcd != NULL);
    // WK_ERROR(device->dcd->ep_pool != NULL);
    // WK_ERROR(ep != NULL);
    // WK_ERROR(ep->ep_desc != NULL);

    while(device->dcd->ep_pool[i].addr != 0xFF)
    {
        if (device->dcd->ep_pool[i].status == ID_UNASSIGNED &&
            ep->ep_desc->bmAttributes == device->dcd->ep_pool[i].type &&
            (EP_ADDRESS(ep) & 0x80) == device->dcd->ep_pool[i].dir) {
            EP_ADDRESS(ep) |= device->dcd->ep_pool[i].addr;
            ep->id = &device->dcd->ep_pool[i];
            device->dcd->ep_pool[i].status = ID_ASSIGNED;

            pr_info("assigned %d\r\n", device->dcd->ep_pool[i].addr);
            return 0;
        }
        i++;
    }

    return -1;
}

int usbd_ep_unassign(udevice_t device, uep_t ep)
{
    // WK_ERROR(device != NULL);
    // WK_ERROR(device->dcd != NULL);
    // WK_ERROR(device->dcd->ep_pool != NULL);
    // WK_ERROR(ep != NULL);
    // WK_ERROR(ep->ep_desc != NULL);

    ep->id->status = ID_UNASSIGNED;

    return 0;
}

int usbd_ep0_setup_handler(udcd_t dcd, struct urequest *setup)
{
    struct udev_msg msg;
    size_t size;

    // WK_ERROR(dcd != NULL);

    if (setup == NULL) {
        size = dcd_ep_read(dcd, EP0_OUT_ADDR, (void*)&msg.content.setup);
        if (size != sizeof(*setup)) {
            pr_err("read setup packet error\r\n");
            return -1;
        }
    } else {
        memcpy((void*)&msg.content.setup, (void*)setup, sizeof(*setup));
    }

    msg.type = USB_MSG_SETUP_NOTIFY;
    msg.dcd = dcd;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_ep0_in_handler(udcd_t dcd)
{
    int32_t remain, mps;

    // WK_ERROR(dcd != NULL);

    if (dcd->stage != STAGE_DIN)
        return 0;

    mps = dcd->ep0.id->maxpacket;
    dcd->ep0.request.remain_size -= mps;
    remain = dcd->ep0.request.remain_size;

    if (remain > 0)
    {
        if (remain >= mps)
        {
            remain = mps;
        }

        dcd->ep0.request.buffer += mps;
        dcd_ep_write(dcd, EP0_IN_ADDR, dcd->ep0.request.buffer, remain);
    }
    else
    {
        /* last packet is MPS multiple, so send ZLP packet */
        if ((remain == 0) && (dcd->ep0.request.size > 0))
        {
            dcd->ep0.request.size = 0;
            dcd_ep_write(dcd, EP0_IN_ADDR, NULL, 0);
        }
        else
        {
            /* receive status */
            dcd->stage = STAGE_STATUS_OUT;
            dcd_ep_read_prepare(dcd, EP0_OUT_ADDR, NULL, 0);
        }
    }

    return 0;
}

int usbd_ep0_out_handler(udcd_t dcd, size_t size)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_EP0_OUT;
    msg.dcd = dcd;
    msg.content.ep_msg.size = size;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_ep_in_handler(udcd_t dcd, uint8_t address, size_t size)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_DATA_NOTIFY;
    msg.dcd = dcd;
    msg.content.ep_msg.ep_addr = address;
    msg.content.ep_msg.size = size;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_ep_out_handler(udcd_t dcd, uint8_t address, size_t size)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_DATA_NOTIFY;
    msg.dcd = dcd;
    msg.content.ep_msg.ep_addr = address;
    msg.content.ep_msg.size = size;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_reset_handler(udcd_t dcd)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_RESET;
    msg.dcd = dcd;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_connect_handler(udcd_t dcd)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_PLUG_IN;
    msg.dcd = dcd;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_disconnect_handler(udcd_t dcd)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_PLUG_OUT;
    msg.dcd = dcd;
    usbd_event_signal(&msg);

    return 0;
}

int usbd_sof_handler(udcd_t dcd)
{
    struct udev_msg msg;

    // WK_ERROR(dcd != NULL);

    msg.type = USB_MSG_SOF;
    msg.dcd = dcd;
    usbd_event_signal(&msg);

    return 0;
}

size_t usbd_ep0_write(udevice_t device, void *buffer, size_t size)
{
    uep_t ep0;
    size_t sent_size = 0;

    // WK_ERROR(device != NULL);
    // WK_ERROR(device->dcd != NULL);
    // WK_ERROR(buffer != NULL);
    // WK_ERROR(size > 0);

    ep0 = &device->dcd->ep0;
    ep0->request.size = size;
    ep0->request.buffer = buffer;
    ep0->request.remain_size = size;
    if(size >= ep0->id->maxpacket)
    {
        sent_size = ep0->id->maxpacket;
    }
    else
    {
        sent_size = size;
    }
    device->dcd->stage = STAGE_DIN;

    return dcd_ep_write(device->dcd, EP0_IN_ADDR, ep0->request.buffer, sent_size);
}

size_t usbd_ep0_read(udevice_t device, void *buffer, size_t size,
    int (*rx_ind)(udevice_t device, size_t size))
{
    uep_t ep0;
    size_t read_size = 0;

    // WK_ERROR(device != NULL);
    // WK_ERROR(device->dcd != NULL);
    // WK_ERROR(buffer != NULL);

    ep0 = &device->dcd->ep0;
    ep0->request.buffer = buffer;
    ep0->request.remain_size = size;
    ep0->rx_indicate = rx_ind;
    if(size >= ep0->id->maxpacket)
        read_size = ep0->id->maxpacket;
    else
        read_size = size;
    device->dcd->stage = STAGE_DOUT;
    dcd_ep_read_prepare(device->dcd, EP0_OUT_ADDR, buffer, read_size);

    return size;
}

static struct msg_queue usb_mq;

/**
 * This function is the main entry of usb device task, it is in charge of
 * processing all messages received from the usb message buffer.
 *
 * @param parameter the parameter of the usb device task.
 *
 * @return none.
 */
static void usbd_task_entry(__maybe_unused void* parameter)
{
    while (1)
    {
        struct udev_msg msg;
        udevice_t device;

        /* receive message */
        if (msg_q_recv(&usb_mq, (char *)&msg, sizeof(struct udev_msg)) <= 0)
            continue;

        device = usbd_find_device(msg.dcd);
        if (device == NULL) {
            pr_err("invalid usb device\r\n");
            continue;
        }

        // pr_info("message type %d\r\n", msg.type);

        switch (msg.type) {
        case USB_MSG_SOF:
            _sof_notify(device);
            break;
        case USB_MSG_DATA_NOTIFY:
            /* some buggy drivers will have USB_MSG_DATA_NOTIFY before the core
             * got configured. */
            _data_notify(device, &msg.content.ep_msg);
            break;
        case USB_MSG_SETUP_NOTIFY:
            _setup_request(device, &msg.content.setup);
            break;
        case USB_MSG_EP0_OUT:
            _ep0_out_notify(device, &msg.content.ep_msg);
            break;
        case USB_MSG_RESET:
            pr_info("reset %d\r\n", device->state);
            if (device->state == USB_STATE_ADDRESS || device->state == USB_STATE_CONFIGURED)
                _stop_notify(device);
            device->state = USB_STATE_NOTATTACHED;
            break;
        case USB_MSG_PLUG_IN:
            device->state = USB_STATE_ATTACHED;
            break;
        case USB_MSG_PLUG_OUT:
            device->state = USB_STATE_NOTATTACHED;
            _stop_notify(device);
            break;
        default:
            pr_err("unknown msg type %d\r\n", msg.type);
            break;
        }
    }
}

/**
 * This function will post an message to usb message queue,
 *
 * @param msg the message to be posted
 * @param size the size of the message .
 *
 * @return the error code, 0 on successfully.
 */
int usbd_event_signal(struct udev_msg *msg)
{
    int rc;

    if (msg == NULL) {
        pr_err("msg is NULL\r\n");
        return -EINVAL;
    }

    /* send message to usb message queue */
    rc = msg_q_send(&usb_mq, (char *)msg, sizeof(struct udev_msg));
    if (rc < 0) {
        pr_err("usb message send failed, rc=%d\r\n", rc);
    }

    return rc;
}


struct task_struct *usb_task;

/**
 * This function will initialize usb device task.
 *
 * @return none.
 *
 */
int usbd_core_init(void)
{
    INIT_LIST_HEAD(&usb_device_list);

    /* create usb device task */
    usb_task = task_create("usbd", usbd_task_entry, NULL, 5, 1024, 5, NULL);

    /* create an usb message queue */
    msg_q_init(&usb_mq);
    mutex_init(&ep_write_lock);

    task_ready(usb_task);

    return 0;
}

int usb_class_register(void)
{
#ifdef CONFIG_USB_HID
    usbd_hid_class_register();
#endif
#ifdef CONFIG_USB_WINUSB
    usbd_winusb_class_register();
#endif
#ifdef CONFIG_USB_CDC
    usbd_vcom_class_register();
#endif

    return 0;
}

