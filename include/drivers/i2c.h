/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_I2C_H__
#define __NOS_DRIVERS_I2C_H__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <stddef.h>
#include <stdint.h>

#define I2C_MSG_READ    (1U << 0)
#define I2C_MSG_RESTART (1U << 1)
#define I2C_MSG_STOP    (1U << 2)

struct i2c_msg {
    uint8_t *buf;
    size_t len;
    uint8_t flags;
};

struct i2c_driver_api {
    /* Serialize all messages; preserve repeated-start and stop flags. */
    int (*transfer)(const struct device *dev, uint16_t address,
                    const struct i2c_msg *messages, size_t count);
};

static inline int i2c_transfer(const struct device *dev, uint16_t address,
                               const struct i2c_msg *messages, size_t count)
{
    const struct i2c_driver_api *api;
    size_t i;

    if (address > 0x7fU || messages == NULL || count == 0) {
        return -EINVAL;
    }
    for (i = 0; i < count; i++) {
        if (messages[i].buf == NULL || messages[i].len == 0 ||
            (messages[i].flags & ~(I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP))) {
            return -EINVAL;
        }
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->transfer == NULL) {
        return -ENOSYS;
    }
    return api->transfer(dev, address, messages, count);
}

#endif /* __NOS_DRIVERS_I2C_H__ */
