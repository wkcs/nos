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
    int (*erase)(const struct device *dev, uint32_t lba, uint32_t count);
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

/**
 * 擦除块设备上指定的扇区范围
 *
 * @param dev 块设备句柄
 * @param lba 起始逻辑块地址
 * @param count 擦除扇区数量
 * @return 0 成功，负数表示错误码
 */
static inline int block_erase(const struct device *dev, uint32_t lba,
                              uint32_t count)
{
    const struct block_driver_api *api;

    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (count == 0) {
        return 0;
    }

    api = dev->api;
    if (api == NULL) {
        return -ENOSYS;
    }

    if (api->erase != NULL) {
        return api->erase(dev, lba, count);
    }

    return 0;
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
