/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#ifndef __NOS_DRIVERS_SERIAL_H__
#define __NOS_DRIVERS_SERIAL_H__

#include <kernel/device.h>
#include <kernel/errno.h>

struct uart_driver_api {
    int (*poll_out)(const struct device *dev, unsigned char out);
    int (*poll_in)(const struct device *dev, unsigned char *in);
};

static inline int uart_poll_out(const struct device *dev, unsigned char out)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }

    api = dev->api;
    if (api == NULL || api->poll_out == NULL) {
        return -ENOSYS;
    }

    return api->poll_out(dev, out);
}

static inline int uart_poll_in(const struct device *dev, unsigned char *in)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev) || in == NULL) {
        return -ENODEV;
    }

    api = dev->api;
    if (api == NULL || api->poll_in == NULL) {
        return -ENOSYS;
    }

    return api->poll_in(dev, in);
}

#endif /* __NOS_DRIVERS_SERIAL_H__ */
