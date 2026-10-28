/** Copyright (C) 2026 Nick Hu */
#include <drivers/i2c_gpio.h>
#include <stdio.h>

static int scl = 1, sda = 1, starts, stops, locked, force_scl_low;
static const uint8_t *reads;
static size_t read_count, read_index;
static uint8_t rising_sda[128];
static size_t rising_count;
int device_init(struct device *dev) { dev->mutex.locked = 0; return 0; }
int device_register(struct device *dev) { (void)dev; return 0; }
void mutex_lock(struct mutex *m) { if (m->locked) locked = -100; else { m->locked = 1; locked++; } }
void mutex_unlock(struct mutex *m) { if (!m->locked) locked = -100; else { m->locked = 0; locked--; } }
static void set_scl(void *ctx, bool high)
{
    (void)ctx;
    if (high && rising_count < sizeof(rising_sda)) rising_sda[rising_count++] = (uint8_t)sda;
    scl = high;
}
static void set_sda(void *ctx, bool high)
{
    (void)ctx;
    if (scl && sda && !high) starts++;
    if (scl && !sda && high) stops++;
    sda = high;
}
static int get_scl(void *ctx) { (void)ctx; return !force_scl_low && scl; }
static int get_sda(void *ctx)
{
    (void)ctx;
    return read_index < read_count ? reads[read_index++] : 1;
}
static void delay(void *ctx) { (void)ctx; }

int main(void)
{
    const struct i2c_gpio_ops ops = { .set_scl = set_scl, .set_sda = set_sda,
                                     .get_scl = get_scl, .get_sda = get_sda, .delay = delay };
    const struct i2c_gpio_config config = { .ops = &ops, .stretch_limit = 3 };
    struct device dev = { .name = "gpio-i2c", .config = &config, .api = &i2c_gpio_driver_api };
    uint8_t command = 0x0f, result = 0;
    struct i2c_msg messages[] = {
        { .buf = &command, .len = 1 },
        { .buf = &result, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP },
    };
    /* Idle bus, address ACK, command ACK, second address ACK, data 0xa5. */
    const uint8_t response[] = { 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1 };
    const uint8_t nack[] = { 1, 1 };
    const uint8_t read_two[] = { 1, 0, 1, 0, 1, 0, 0, 1, 0, 1,
                                 0, 0, 1, 1, 1, 1, 0, 0 };
    const uint8_t busy_bus[] = { 0 };
    uint8_t result_two[2] = { 0 };
    struct i2c_msg read_message = { .buf = result_two, .len = 2,
                                    .flags = I2C_MSG_READ | I2C_MSG_STOP };

    if (i2c_gpio_init(&dev) != 0 || !dev.ready) return 1;
    starts = stops = 0; reads = response; read_count = sizeof(response); read_index = 0;
    if (i2c_transfer(&dev, 0x3c, messages, 2) != 0 || result != 0xa5 ||
        starts != 2 || stops != 1 || locked || read_index != sizeof(response)) {
        fputs("combined I2C transaction failed\n", stderr); return 2;
    }
    starts = stops = 0; reads = read_two; read_count = sizeof(read_two); read_index = 0;
    rising_count = 0;
    if (i2c_transfer(&dev, 0x3c, &read_message, 1) != 0 ||
        result_two[0] != 0xa5 || result_two[1] != 0x3c || rising_count < 28 ||
        rising_sda[18] != 0 || rising_sda[27] != 1 || locked) {
        fputs("I2C read ACK/NACK sequence failed\n", stderr); return 5;
    }
    starts = stops = 0; reads = busy_bus; read_count = sizeof(busy_bus); read_index = 0;
    if (i2c_transfer(&dev, 0x3c, messages, 1) != -EBUSY || starts != 0 || locked) {
        fputs("busy I2C bus was not rejected\n", stderr); return 6;
    }
    starts = stops = 0; reads = nack; read_count = sizeof(nack); read_index = 0;
    if (i2c_transfer(&dev, 0x3c, messages, 1) != -ENXIO || stops != 1 || locked) {
        fputs("I2C NACK handling failed\n", stderr); return 3;
    }
    force_scl_low = 1; read_count = read_index = 0;
    if (i2c_transfer(&dev, 0x3c, messages, 1) != -ETIMEDOUT || locked) {
        fputs("I2C clock-stretch timeout failed\n", stderr); return 4;
    }
    puts("GPIO I2C checks passed");
    return 0;
}
