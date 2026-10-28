/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#ifndef __NOS_DRIVERS_BLOCK_H__
#define __NOS_DRIVERS_BLOCK_H__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <stdint.h>

struct block_driver_api {
    int (*read)(const struct device *dev, uint32_t lba, void *buf,
                uint32_t count);
    int (*write)(const struct device *dev, uint32_t lba, const void *buf,
                 uint32_t count);
    int (*get_info)(const struct device *dev, uint32_t *sector_size,
                    uint32_t *sector_count);
};

static inline int block_read(const struct device *dev, uint32_t lba, void *buf,
                             uint32_t count)
{
    const struct block_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (buf == NULL) {
        return -EINVAL;
    }

    api = dev->api;
    if (api == NULL || api->read == NULL) {
        return -ENOSYS;
    }

    return api->read(dev, lba, buf, count);
}

static inline int block_write(const struct device *dev, uint32_t lba,
                              const void *buf, uint32_t count)
{
    const struct block_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (buf == NULL) {
        return -EINVAL;
    }

    api = dev->api;
    if (api == NULL || api->write == NULL) {
        return -ENOSYS;
    }

    return api->write(dev, lba, buf, count);
}

static inline int block_get_info(const struct device *dev,
                                 uint32_t *sector_size,
                                 uint32_t *sector_count)
{
    const struct block_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (sector_size == NULL || sector_count == NULL) {
        return -EINVAL;
    }

    api = dev->api;
    if (api == NULL || api->get_info == NULL) {
        return -ENOSYS;
    }

    return api->get_info(dev, sector_size, sector_count);
}

#endif /* __NOS_DRIVERS_BLOCK_H__ */
