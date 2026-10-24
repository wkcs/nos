/* Copyright (C) 2026 NOS contributors. SDK enumeration/URBs, NOS HID and BOT. */
#include <stdlib.h>
#include <string.h>
#include "private.h"
#include "nos/driver/usb_service.h"
#include "usb/usb_host.h"
#include "esp_timer.h"
struct host_context {
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    usb_transfer_t *transfer;
    struct nos_usb_host description;
    struct nos_usb_hid hid;
    struct nos_usb_msc msc;
    uint8_t claimed[NOS_USB_INTERFACES], claim_count;
    uint8_t pending, done, gone, address, available, hid_open, msc_open, faulted, root_off;
};
static uint32_t generation;
static int advance(struct host_context *c)
{
    c->description.configured = c->hid_open = c->msc_open = 0;
    if (generation == UINT32_MAX) {
        c->faulted = 1;
        return -NOS_DRV_ENOSPC;
    }
    c->description.generation = ++generation;
    return 0;
}
static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    struct host_context *c = arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV && !c->available)
        c->available = event->new_dev.address;
    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE && event->dev_gone.dev_hdl == c->device) {
        c->gone = 1;
        if (c->available == c->address)
            c->available = 0;
        (void)advance(c);
    }
}
static int pump(struct host_context *c)
{
    uint32_t flags;
    int rc = usb_host_lib_handle_events(0, &flags);
    if (rc != ESP_OK && rc != ESP_ERR_TIMEOUT)
        return idf_error(rc);
    rc = usb_host_client_handle_events(c->client, 0);
    return rc == ESP_ERR_TIMEOUT ? 0 : idf_error(rc);
}
static void completed(usb_transfer_t *transfer)
{
    struct host_context *c = transfer->context;
    c->pending = 0;
    c->done = 1;
}
static int alive(struct host_context *c, uint32_t token)
{
    if (!c->device || c->gone || !c->description.configured || !token ||
        token != c->description.generation)
        return -NOS_DRV_ENODEV;
    return c->faulted ? -NOS_DRV_EIO : 0;
}
static int describe(void *context, uint32_t token, struct nos_usb_class_info *info)
{
    struct host_context *c = context;
    int rc = alive(c, token);
    if (rc)
        return rc;
    *info = (struct nos_usb_class_info){.interface_count = c->description.interface_count,
                                        .endpoint_count = c->description.endpoint_count,
                                        .max_transfer = 4096};
    for (unsigned i = 0; i < info->interface_count; i++)
        info->interfaces[i] = c->description.interfaces[i];
    for (unsigned i = 0; i < info->endpoint_count; i++)
        info->endpoints[i] = c->description.endpoints[i];
    return 0;
}
static int wait_transfer(struct host_context *c, uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;
    for (unsigned loops = 0; c->pending && loops < 100000; loops++) {
        int rc = pump(c);
        if (rc)
            return rc;
        if (!c->pending)
            break;
        if (esp_timer_get_time() >= deadline)
            return -NOS_DRV_ETIMEDOUT;
        vTaskDelay(1);
    }
    return c->pending ? -NOS_DRV_ETIMEDOUT : 0;
}
static int cancel(struct host_context *c)
{
    if (!c->pending)
        return 0;
    int rc = pump(c);
    if (rc)
        return rc;
    if (!c->pending)
        return 0;
    uint8_t ep = c->transfer->bEndpointAddress;
    /* Public endpoint commands exclude EP0. Stop the owned root port to
     * disconnect/quiesce a timed-out control transfer, rather than freeing an
     * in-flight URB. The same fallback handles failed endpoint cancellation. */
    rc = ep ? idf_error(usb_host_endpoint_halt(c->device, ep)) : -NOS_DRV_ENOTSUP;
    if (!rc)
        rc = idf_error(usb_host_endpoint_flush(c->device, ep));
    if (rc && !c->root_off) {
        rc = idf_error(usb_host_lib_set_root_port_power(false));
        if (!rc)
            c->root_off = 1;
    }
    if (c->root_off)
        rc = 0;
    if (!rc)
        rc = wait_transfer(c, 1000000);
    if (rc)
        c->faulted = 1;
    return rc;
}
static int exchange(struct host_context *c, uint32_t token, uint8_t ep, const uint8_t *setup,
                    void *data, size_t length, size_t *actual, uint32_t timeout_us)
{
    *actual = 0;
    int rc = alive(c, token);
    if (rc)
        return rc;
    if (c->pending || !c->transfer)
        return -NOS_DRV_EBUSY;
    if (length > 4096 || (length && !data) || !timeout_us || timeout_us > INT32_MAX)
        return -NOS_DRV_EINVAL;
    int input = setup ? (setup[0] & 0x80) : (ep & 0x80);
    uint8_t endpoint_type = NOS_USB_CONTROL;
    size_t prefix = setup ? 8 : 0, requested = length;
    if (!setup) {
        struct nos_usb_endpoint *endpoint = NULL;
        for (unsigned i = 0; i < c->description.endpoint_count; i++)
            if (c->description.endpoints[i].address == ep)
                endpoint = &c->description.endpoints[i];
        if (!endpoint)
            return -NOS_DRV_ENODEV;
        endpoint_type = endpoint->type;
        if (endpoint->halted)
            return -NOS_USB_EPIPE;
        if (input)
            requested =
                (length + endpoint->max_packet - 1) / endpoint->max_packet * endpoint->max_packet;
    } else {
        if ((size_t)(setup[6] | ((unsigned)setup[7] << 8)) != length)
            return -NOS_DRV_EINVAL;
        unsigned mps = c->description.ep0_size;
        if (!mps)
            return -NOS_DRV_EIO;
        if (input)
            requested = (length + mps - 1) / mps * mps;
    }
    if (prefix + requested > c->transfer->data_buffer_size)
        return -NOS_DRV_EINVAL;
    if (setup)
        memcpy(c->transfer->data_buffer, setup, 8);
    if (!input && length)
        memcpy(c->transfer->data_buffer + prefix, data, length);
    usb_transfer_t *t = c->transfer;
    t->device_handle = c->device;
    t->bEndpointAddress = ep;
    t->num_bytes = prefix + requested;
    t->callback = completed;
    t->context = c;
    c->done = 0;
    c->pending = 1;
    rc = idf_error(setup ? usb_host_transfer_submit_control(c->client, t)
                         : usb_host_transfer_submit(t));
    if (rc) {
        c->pending = 0;
        return rc;
    }
    rc = wait_transfer(c, timeout_us);
    if (rc) {
        int canceled = cancel(c);
        if (canceled || c->root_off || rc != -NOS_DRV_ETIMEDOUT) {
            c->faulted = 1;
            return rc; /* Retain bounce storage and controller ownership. */
        }
        if (c->gone)
            return -NOS_DRV_ENODEV;
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
            /* No interrupt report is normal for an idle keyboard/mouse.
             * Retire and reactivate the pipe without poisoning the device. */
            if (input && endpoint_type == NOS_USB_INTERRUPT) {
                int clear = idf_error(usb_host_endpoint_clear(c->device, ep));
                if (!clear)
                    return -NOS_DRV_EAGAIN;
                c->faulted = 1;
                return clear;
            }
            for (unsigned i = 0; i < c->description.endpoint_count; i++)
                if (c->description.endpoints[i].address == ep)
                    c->description.endpoints[i].halted = 1;
            return rc; /* BOT requires explicit reset, never replay a write. */
        }
    }
    if (c->gone || token != c->description.generation)
        return -NOS_DRV_ENODEV;
    if (t->status == USB_TRANSFER_STATUS_STALL) {
        for (unsigned i = 0; i < c->description.endpoint_count; i++)
            if (c->description.endpoints[i].address == ep)
                c->description.endpoints[i].halted = 1;
        return -NOS_USB_EPIPE;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED)
        return -NOS_DRV_EIO;
    if (t->actual_num_bytes < (int)prefix || (size_t)t->actual_num_bytes - prefix > length)
        return -NOS_DRV_EIO;
    *actual = t->actual_num_bytes - prefix;
    if (input && *actual)
        memcpy(data, t->data_buffer + prefix, *actual);
    return 0;
}
static int control(void *c, uint32_t g, const uint8_t setup[8], void *data, size_t n, size_t *a,
                   uint32_t t)
{
    return exchange(c, g, 0, setup, data, n, a, t);
}
static int transfer(void *c, uint32_t g, uint8_t e, void *data, size_t n, size_t *a, uint32_t t)
{
    return exchange(c, g, e, NULL, data, n, a, t);
}
static int clear_halt(void *context, uint32_t token, uint8_t ep)
{
    struct host_context *c = context;
    uint8_t setup[8] = {2, 1, 0, 0, ep, 0, 0, 0};
    size_t actual;
    int rc = alive(c, token);
    if (!rc && c->pending)
        rc = -NOS_DRV_EBUSY;
    if (!rc)
        rc = idf_error(usb_host_endpoint_halt(c->device, ep));
    if (!rc)
        rc = idf_error(usb_host_endpoint_flush(c->device, ep));
    if (!rc)
        rc = control(c, token, setup, NULL, 0, &actual, 500000);
    if (!rc)
        rc = idf_error(usb_host_endpoint_clear(c->device, ep));
    if (!rc)
        for (unsigned i = 0; i < c->description.endpoint_count; i++)
            if (c->description.endpoints[i].address == ep)
                c->description.endpoints[i].halted = 0;
    return rc;
}
static const struct nos_usb_transport_ops io_ops = {describe, control, transfer, clear_halt};
static int detach(struct host_context *c)
{
    int rc = cancel(c);
    if (rc)
        return rc;
    if (!c->device)
        return 0;
    while (c->claim_count) {
        rc = idf_error(
            usb_host_interface_release(c->client, c->device, c->claimed[c->claim_count - 1]));
        if (rc)
            return rc;
        c->claim_count--;
    }
    rc = idf_error(usb_host_device_close(c->client, c->device));
    if (rc)
        return rc;
    c->device = NULL;
    c->gone = c->faulted = 0;
    return advance(c);
}
static int host_start(void *context)
{
    struct idf_device *d = context;
    struct host_context *c = calloc(1, sizeof(*c));
    if (!c)
        return -NOS_DRV_ENOSPC;
    d->u.usb = c;
    usb_host_config_t config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    int rc = idf_error(usb_host_install(&config));
    if (rc)
        return rc;
    d->installed = 1;
    usb_host_client_config_t client = {
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event, .callback_arg = c}};
    rc = idf_error(usb_host_client_register(&client, &c->client));
    if (!rc)
        rc = idf_error(usb_host_transfer_alloc(4160, 0, &c->transfer));
    return rc;
}
static int host_stop(void *context)
{
    struct idf_device *d = context;
    struct host_context *c = d->u.usb;
    if (c) {
        int rc = detach(c);
        if (rc)
            return rc;
        if (c->transfer) {
            rc = idf_error(usb_host_transfer_free(c->transfer));
            if (rc)
                return rc;
            c->transfer = NULL;
        }
        if (c->client) {
            rc = idf_error(usb_host_client_deregister(c->client));
            if (rc)
                return rc;
            c->client = NULL;
        }
    }
    if (d->installed) {
        int result = usb_host_device_free_all();
        if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED)
            return idf_error(result);
        uint32_t flags;
        result = usb_host_lib_handle_events(0, &flags);
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT)
            return idf_error(result);
        int rc = idf_error(usb_host_uninstall());
        if (rc)
            return rc;
        d->installed = 0;
    }
    free(c);
    d->u.usb = NULL;
    return 0;
}
static int attach(struct host_context *c, struct nos_usb_attach *a)
{
    if (c->device || c->faulted || c->root_off)
        return -NOS_DRV_EBUSY;
    int rc = pump(c);
    if (rc)
        return rc;
    uint8_t address = a->address ? a->address : c->available;
    if (!address)
        return -NOS_DRV_EAGAIN;
    rc = idf_error(usb_host_device_open(c->client, address, &c->device));
    if (rc)
        return rc;
    c->address = address;
    c->gone = 0;
    const usb_device_desc_t *device;
    const usb_config_desc_t *config;
    usb_device_info_t info;
    rc = idf_error(usb_host_get_device_descriptor(c->device, &device));
    if (!rc)
        rc = idf_error(usb_host_device_info(c->device, &info));
    if (!rc)
        rc = idf_error(usb_host_get_active_config_descriptor(c->device, &config));
    if (!rc) {
        c->description.speed = info.speed == USB_SPEED_LOW ? NOS_USB_LOW : NOS_USB_FULL;
        c->description.ep0_size = device->bMaxPacketSize0;
        if (device->bMaxPacketSize0 != 8 && device->bMaxPacketSize0 != 16 &&
            device->bMaxPacketSize0 != 32 && device->bMaxPacketSize0 != 64)
            rc = -NOS_DRV_EINVAL;
        c->description.power_budget_ma = 500;
        c->description.controller.max_transfer = 4096;
        if (!rc)
            rc = nos_usb_host_parse_configuration(&c->description, (const uint8_t *)config,
                                                  config->wTotalLength);
    }
    if (!rc)
        rc = advance(c);
    if (!rc) {
        c->description.vendor = device->idVendor;
        c->description.product = device->idProduct;
        c->description.configured = 1;
        a->generation = c->description.generation;
        a->address = address;
    } else {
        int close = detach(c);
        if (close)
            c->faulted = 1;
    }
    return rc;
}
static int claim_interface(struct host_context *c, uint8_t number)
{
    for (unsigned i = 0; i < c->claim_count; i++)
        if (c->claimed[i] == number)
            return 0;
    if (c->claim_count == NOS_USB_INTERFACES)
        return -NOS_DRV_ENOSPC;
    int rc = idf_error(usb_host_interface_claim(c->client, c->device, number, 0));
    if (!rc)
        c->claimed[c->claim_count++] = number;
    return rc;
}
static int host_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    struct host_context *c = d->u.usb;
    if (!c || !c->client)
        return -NOS_DRV_ENODEV;
    if (op == NOS_IO_POLL && size == sizeof(uint32_t)) {
        int rc = pump(c);
        *(uint32_t *)request = c->gone        ? NOS_USB_PORT_CHANGED
                               : c->available ? NOS_USB_PORT_CONNECTED
                                              : 0;
        return rc;
    }
    if (op == NOS_IO_USB_ATTACH && size == sizeof(struct nos_usb_attach))
        return attach(c, request);
    if (op == NOS_IO_USB_DETACH && !size)
        return detach(c);
    if (op == NOS_IO_USB_INFO && size == sizeof(struct nos_usb_device_info)) {
        struct nos_usb_device_info *info = request;
        *info = (struct nos_usb_device_info){.generation = c->description.generation,
                                             .vendor = c->description.vendor,
                                             .product = c->description.product,
                                             .address = c->address,
                                             .speed = c->description.speed,
                                             .connected = c->device && !c->gone &&
                                                          c->description.configured};
        if (!info->connected)
            return -NOS_DRV_ENODEV;
        info->blocks = c->msc.blocks;
        info->block_size = c->msc.block_size;
        return describe(c, info->generation, &info->classes);
    }
    struct nos_usb_transport io = {&io_ops, c};
    if ((op == NOS_IO_USB_HID_OPEN || op == NOS_IO_USB_MSC_OPEN) &&
        size == sizeof(struct nos_usb_class_open)) {
        struct nos_usb_class_open *o = request;
        int rc = alive(c, o->generation);
        if (rc)
            return rc;
        rc = claim_interface(c, o->interface_number);
        if (rc)
            return rc;
        if (op == NOS_IO_USB_HID_OPEN) {
            c->hid_open = 0;
            rc = nos_usb_hid_open_transport(&c->hid, &io, o->generation, o->interface_number);
            if (!rc)
                c->hid_open = 1;
        } else {
            c->msc_open = 0;
            rc = nos_usb_msc_open_transport(&c->msc, &io, o->generation, o->interface_number);
            if (!rc)
                c->msc_open = 1;
        }
        return rc;
    }
    if (op == NOS_IO_USB_HID_READ && size == sizeof(struct nos_usb_report)) {
        struct nos_usb_report *r = request;
        r->actual = 0;
        int rc = alive(c, r->generation);
        if (rc)
            return rc;
        return c->hid_open ? nos_usb_hid_read(&c->hid, r->data, r->capacity, &r->actual)
                           : -NOS_DRV_ENODEV;
    }
    if (op == NOS_IO_USB_HID_LEDS && size == sizeof(struct nos_usb_leds)) {
        struct nos_usb_leds *l = request;
        int rc = alive(c, l->generation);
        if (rc)
            return rc;
        return c->hid_open ? nos_usb_hid_set_leds(&c->hid, l->leds) : -NOS_DRV_ENODEV;
    }
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_usb_storage_io)) {
        struct nos_usb_storage_io *r = request;
        r->actual = 0;
        int rc = alive(c, r->generation);
        if (rc)
            return rc;
        if (!c->msc_open)
            return -NOS_DRV_ENODEV;
        if (r->sector > UINT32_MAX || !r->count || r->count > UINT16_MAX)
            return -NOS_DRV_EINVAL;
        rc = op == NOS_IO_READ ? nos_usb_msc_read(&c->msc, r->sector, r->count, r->data)
                               : nos_usb_msc_write(&c->msc, r->sector, r->count, r->data);
        if (!rc)
            r->actual = r->count;
        return rc;
    }
    if (op == NOS_IO_COMMIT && !size)
        return c->msc_open ? nos_usb_msc_sync(&c->msc) : -NOS_DRV_ENODEV;
    if (op == NOS_IO_RECOVER && !size)
        return c->msc_open ? nos_usb_msc_recover(&c->msc) : -NOS_DRV_ENODEV;
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_usb_host_ops = {host_start, host_stop, host_call};
