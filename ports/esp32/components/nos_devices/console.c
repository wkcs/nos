/* Copyright (C) 2026 NOS contributors. NOS owns the boot UART0 console. */
#include "private.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
static uint8_t installed;
static int start(void *context) {
    (void)context;
    if (uart_is_driver_installed(UART_NUM_0))
        return -NOS_DRV_EBUSY;
    int rc = idf_error(uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0));
    if (rc)
        return rc;
    installed = 1;
    /* Small interactive requests must not depend on an emulated RX timeout. */
    rc = idf_error(uart_set_rx_full_threshold(UART_NUM_0, 1));
    if (!rc)
        uart_vfs_dev_use_driver(UART_NUM_0);
    return rc;
}
static int stop(void *context) {
    (void)context;
    /* Includes a partially failed start: retain ownership rather than handing
     * an uncertain live boot console to another driver. Reboot is required. */
    return installed ? -NOS_DRV_EBUSY : 0;
}
static int call(void *context, uint32_t op, void *request, size_t size) {
    (void)context;
    if (!installed)
        return -NOS_DRV_ENODEV;
    if ((op != NOS_IO_READ && op != NOS_IO_WRITE) || size != sizeof(struct nos_io_buffer))
        return -NOS_DRV_ENOTSUP;
    struct nos_io_buffer *b = request;
    b->actual = 0;
    if ((b->size && !b->data) || b->size > NOS_DRIVER_MAX_TRANSFER)
        return -NOS_DRV_EINVAL;
    if (!b->size)
        return 0;
    int n = op == NOS_IO_READ ? uart_read_bytes(UART_NUM_0, b->data, b->size, 0)
                              : uart_tx_chars(UART_NUM_0, b->data, b->size);
    if (n < 0 || (size_t)n > b->size)
        return -NOS_DRV_EIO;
    b->actual = n;
    return n ? 0 : -NOS_DRV_EAGAIN;
}
const struct nos_service_ops idf_console_ops = {start, stop, call};
