/* Copyright (C) 2026 NOS contributors. Fixed USB Serial/JTAG CDC, not USB OTG. */
#include "private.h"
#include "driver/usb_serial_jtag.h"
#include <limits.h>
static int start(void *context) {
    struct idf_device *d = context;
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 256, .tx_buffer_size = 256};
    int rc = idf_error(usb_serial_jtag_driver_install(&config));
    if (!rc)
        d->installed = 1;
    return rc;
}
static int stop(void *context) {
    struct idf_device *d = context;
    if (!d->installed)
        return 0;
    int rc = idf_error(usb_serial_jtag_driver_uninstall());
    if (!rc)
        d->installed = 0;
    return rc;
}
static int call(void *context, uint32_t op, void *request, size_t size) {
    struct idf_device *d = context;
    if (!d->installed)
        return -NOS_DRV_ENODEV;
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_buffer)) {
        struct nos_io_buffer *b = request;
        b->actual = 0;
        if ((b->size && !b->data) || b->size > NOS_DRIVER_MAX_TRANSFER)
            return -NOS_DRV_EINVAL;
        if (!b->size)
            return 0;
        int n = op == NOS_IO_READ ? usb_serial_jtag_read_bytes(b->data, b->size, 0)
                                  : usb_serial_jtag_write_bytes(b->data, b->size, 0);
        if (n < 0 || (size_t)n > b->size)
            return -NOS_DRV_EIO;
        b->actual = n;
        return n ? 0 : -NOS_DRV_EAGAIN;
    }
    if (op == NOS_IO_COMMIT && size == sizeof(uint32_t)) {
        uint32_t us = *(uint32_t *)request;
        if (us > 1000000)
            return -NOS_DRV_EINVAL;
        TickType_t ticks = pdMS_TO_TICKS((us + 999) / 1000);
        if (us && !ticks)
            ticks = 1;
        return idf_error(usb_serial_jtag_wait_tx_done(ticks));
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_usb_serial_ops = {start, stop, call};
