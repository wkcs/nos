/* Copyright (C) 2026 NOS contributors. One owner of the shared S3 USB PHY. */
#include "private.h"
#include "usb/usb_host.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
static int host_start(void *context)
{
    struct idf_device *d = context;
    /* No VBUS GPIO guessed here. Board policy must supply external power. */
    usb_host_config_t config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    int rc = idf_error(usb_host_install(&config));
    if (!rc)
        d->installed = 1;
    return rc;
}
static int host_stop(void *context)
{
    struct idf_device *d = context;
    if (!d->installed)
        return 0;
    usb_host_lib_info_t info;
    int rc = idf_error(usb_host_lib_info(&info));
    if (rc)
        return rc;
    if (info.num_clients || info.num_devices)
        return -NOS_DRV_EBUSY; /* Keep ownership until class clients detach. */
    rc = idf_error(usb_host_uninstall());
    if (!rc)
        d->installed = 0;
    return rc;
}
static int host_call(void *context, uint32_t op, void *request, size_t size)
{
    (void)context;
    if (op == NOS_IO_POLL && size == sizeof(uint32_t)) {
        int result = usb_host_lib_handle_events(0, request);
        return result == ESP_ERR_TIMEOUT ? -NOS_DRV_EAGAIN : idf_error(result);
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_usb_host_ops = {host_start, host_stop, host_call};
static int device_start(void *context)
{
    struct idf_device *d = context;
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.task.xCoreID = 0;
    int rc = idf_error(tinyusb_driver_install(&config));
    if (!rc)
        d->installed = 1;
    return rc;
}
static int device_stop(void *context)
{
    struct idf_device *d = context;
    int rc = d->installed ? idf_error(tinyusb_driver_uninstall()) : 0;
    if (!rc)
        d->installed = 0;
    return rc;
}
static int device_call(void *context, uint32_t op, void *request, size_t size)
{
    (void)context;
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_buffer)) {
        struct nos_io_buffer *b = request;
        b->actual = 0;
        if ((b->size && !b->data) || b->size > NOS_DRIVER_MAX_TRANSFER)
            return -NOS_DRV_EINVAL;
        if (!tud_vendor_mounted())
            return -NOS_DRV_ENODEV;
        if (!b->size)
            return 0;
        b->actual = op == NOS_IO_READ ? tud_vendor_read(b->data, b->size)
                                      : tud_vendor_write(b->data, b->size);
        if (op == NOS_IO_WRITE)
            tud_vendor_flush();
        return b->actual ? 0 : -NOS_DRV_EAGAIN;
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_usb_device_ops = {device_start, device_stop, device_call};
