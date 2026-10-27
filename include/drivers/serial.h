/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#ifndef __NOS_DRIVERS_SERIAL_H__
#define __NOS_DRIVERS_SERIAL_H__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <stdint.h>
#include <limits.h>

#define NOS_UART_BULK_API 1
#define NOS_UART_STATS_API 1

struct uart_stats {
    uint32_t rx_dropped;
    uint32_t tx_dropped_records;
    uint32_t dma_errors;
};

struct uart_driver_api {
    int (*poll_out)(const struct device *dev, unsigned char out);
    int (*poll_in)(const struct device *dev, unsigned char *in);
    int (*write)(const struct device *dev, const unsigned char *buf,
                 unsigned int len);
    int (*write_record)(const struct device *dev, const unsigned char *buf,
                        unsigned int len);
    int (*write_recordv)(const struct device *dev, const unsigned char *first,
                         unsigned int first_len, const unsigned char *second,
                         unsigned int second_len);
    int (*write_room)(const struct device *dev);
    int (*get_stats)(const struct device *dev, struct uart_stats *stats);
    int (*read)(const struct device *dev, unsigned char *buf,
                unsigned int len, bool wait);
    int (*panic_begin)(const struct device *dev);
    int (*panic_write)(const struct device *dev, const unsigned char *buf,
                       unsigned int len);
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

    if (in == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }

    api = dev->api;
    if (api == NULL || api->poll_in == NULL) {
        return -ENOSYS;
    }

    return api->poll_in(dev, in);
}

static inline int uart_write(const struct device *dev, const unsigned char *buf,
                             unsigned int len)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->write == NULL) {
        return -ENOSYS;
    }
    return api->write(dev, buf, len);
}

static inline int uart_write_record(const struct device *dev,
                                    const unsigned char *buf, unsigned int len)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->write_record == NULL) {
        return -ENOSYS;
    }
    return api->write_record(dev, buf, len);
}

static inline int uart_read(const struct device *dev, unsigned char *buf,
                            unsigned int len, bool wait)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->read == NULL) {
        return -ENOSYS;
    }
    return api->read(dev, buf, len, wait);
}

static inline int uart_write_recordv(const struct device *dev,
                                     const unsigned char *first, unsigned int first_len,
                                     const unsigned char *second, unsigned int second_len)
{
    const struct uart_driver_api *api;

    if ((first == NULL && first_len != 0) || (second == NULL && second_len != 0) ||
        first_len > UINT_MAX - second_len || first_len + second_len > INT_MAX) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->write_recordv == NULL) {
        return -ENOSYS;
    }
    return api->write_recordv(dev, first, first_len, second, second_len);
}

static inline int uart_write_room(const struct device *dev)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->write_room == NULL) {
        return -ENOSYS;
    }
    return api->write_room(dev);
}

static inline int uart_get_stats(const struct device *dev, struct uart_stats *stats)
{
    const struct uart_driver_api *api;

    if (stats == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->get_stats == NULL) {
        return -ENOSYS;
    }
    return api->get_stats(dev, stats);
}

static inline int uart_panic_begin(const struct device *dev)
{
    const struct uart_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->panic_begin == NULL) {
        return -ENOSYS;
    }
    return api->panic_begin(dev);
}

static inline int uart_panic_write(const struct device *dev,
                                   const unsigned char *buf, unsigned int len)
{
    const struct uart_driver_api *api;

    if (buf == NULL && len != 0) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->panic_write == NULL) {
        return -ENOSYS;
    }
    return api->panic_write(dev, buf, len);
}

#endif /* __NOS_DRIVERS_SERIAL_H__ */
