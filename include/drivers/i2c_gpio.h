/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_I2C_GPIO_H__
#define __NOS_DRIVERS_I2C_GPIO_H__

#include <drivers/i2c.h>
#include <stdbool.h>

/* Single-master bus: a high output releases an open-drain line, never drives it. */
struct i2c_gpio_ops {
    void (*set_scl)(void *context, bool high);
    void (*set_sda)(void *context, bool high);
    int (*get_scl)(void *context);
    int (*get_sda)(void *context);
    void (*delay)(void *context);
};

struct i2c_gpio_config {
    const struct i2c_gpio_ops *ops;
    void *context;
    uint32_t stretch_limit;
};

extern const struct i2c_driver_api i2c_gpio_driver_api;
int i2c_gpio_init(struct device *dev);

#endif /* __NOS_DRIVERS_I2C_GPIO_H__ */
