/** Copyright (C) 2026 Nick Hu */
#include <drivers/i2c.h>
#include <stdio.h>

static int calls;
static int transfer(const struct device *dev, uint16_t address,
                    const struct i2c_msg *messages, size_t count)
{
    (void)dev;
    calls++;
    if (address != 0x3c || count != 2 || messages[0].flags != 0 ||
        messages[1].flags != (I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP)) {
        return -EINVAL;
    }
    messages[1].buf[0] = 0x42;
    return 0;
}

int main(void)
{
    const struct i2c_driver_api api = { .transfer = transfer };
    struct device dev = { .api = &api, .ready = true };
    uint8_t command = 1;
    uint8_t data = 0;
    struct i2c_msg messages[] = {
        { .buf = &command, .len = 1, .flags = 0 },
        { .buf = &data, .len = 1,
          .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP },
    };
    if (i2c_transfer(&dev, 0x3c, messages, 2) != 0 || data != 0x42 || calls != 1) {
        fputs("combined transfer failed\n", stderr);
        return 1;
    }
    if (i2c_transfer(&dev, 0x80, messages, 2) != -EINVAL ||
        i2c_transfer(&dev, 0x3c, NULL, 2) != -EINVAL ||
        i2c_transfer(&dev, 0x3c, messages, 0) != -EINVAL || calls != 1) {
        fputs("invalid transfer accepted\n", stderr);
        return 1;
    }
    dev.ready = false;
    if (i2c_transfer(&dev, 0x3c, messages, 2) != -ENODEV) {
        fputs("unready controller accepted\n", stderr);
        return 1;
    }
    puts("I2C API checks passed");
    return 0;
}
