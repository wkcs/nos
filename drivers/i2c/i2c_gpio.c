/** Copyright (C) 2026 Nick Hu */
#include <drivers/i2c_gpio.h>

static int i2c_gpio_clock_high(const struct i2c_gpio_config *cfg)
{
    uint32_t remaining = cfg->stretch_limit;
    int level;

    cfg->ops->set_scl(cfg->context, true);
    do {
        cfg->ops->delay(cfg->context);
        level = cfg->ops->get_scl(cfg->context);
        if (level < 0) return level;
        if (level != 0) return 0;
    } while (--remaining != 0);
    return -ETIMEDOUT;
}

static int i2c_gpio_start(const struct i2c_gpio_config *cfg, bool repeated)
{
    int ret;

    cfg->ops->set_sda(cfg->context, true);
    ret = i2c_gpio_clock_high(cfg);
    if (ret != 0) return ret;
    if (!repeated) {
        ret = cfg->ops->get_sda(cfg->context);
        if (ret < 0) return ret;
        if (ret == 0) return -EBUSY;
    }
    cfg->ops->set_sda(cfg->context, false);
    cfg->ops->delay(cfg->context);
    cfg->ops->set_scl(cfg->context, false);
    return 0;
}

static int i2c_gpio_stop(const struct i2c_gpio_config *cfg)
{
    int ret;

    cfg->ops->set_scl(cfg->context, false);
    cfg->ops->set_sda(cfg->context, false);
    cfg->ops->delay(cfg->context);
    ret = i2c_gpio_clock_high(cfg);
    cfg->ops->set_sda(cfg->context, true);
    cfg->ops->delay(cfg->context);
    return ret;
}

static int i2c_gpio_write_bit(const struct i2c_gpio_config *cfg, bool bit)
{
    int ret;

    cfg->ops->set_sda(cfg->context, bit);
    cfg->ops->delay(cfg->context);
    ret = i2c_gpio_clock_high(cfg);
    cfg->ops->set_scl(cfg->context, false);
    return ret;
}

static int i2c_gpio_write_byte(const struct i2c_gpio_config *cfg, uint8_t value)
{
    unsigned int bit;
    int ret;

    for (bit = 0; bit < 8; bit++) {
        ret = i2c_gpio_write_bit(cfg, (value & 0x80U) != 0);
        if (ret != 0) return ret;
        value <<= 1;
    }
    cfg->ops->set_sda(cfg->context, true);
    ret = i2c_gpio_clock_high(cfg);
    if (ret == 0) {
        ret = cfg->ops->get_sda(cfg->context);
        if (ret > 0) ret = -ENXIO;
    }
    cfg->ops->set_scl(cfg->context, false);
    return ret;
}

static int i2c_gpio_read_byte(const struct i2c_gpio_config *cfg,
                              uint8_t *value, bool last)
{
    unsigned int bit;
    uint8_t result = 0;
    int ret;

    cfg->ops->set_sda(cfg->context, true);
    for (bit = 0; bit < 8; bit++) {
        ret = i2c_gpio_clock_high(cfg);
        if (ret == 0) ret = cfg->ops->get_sda(cfg->context);
        cfg->ops->set_scl(cfg->context, false);
        cfg->ops->delay(cfg->context);
        if (ret < 0) return ret;
        result = (uint8_t)((result << 1) | (ret != 0));
    }
    ret = i2c_gpio_write_bit(cfg, last);
    if (ret == 0) *value = result;
    return ret;
}

static int i2c_gpio_transfer(const struct device *dev, uint16_t address,
                             const struct i2c_msg *messages, size_t count)
{
    struct device *controller = (struct device *)dev;
    const struct i2c_gpio_config *cfg = dev->config;
    bool active = false;
    bool read = false;
    size_t i;
    size_t j;
    int ret = 0;

    mutex_lock(&controller->mutex);
    for (i = 0; i < count && ret == 0; i++) {
        bool msg_read = (messages[i].flags & I2C_MSG_READ) != 0;
        if (!active || (messages[i].flags & I2C_MSG_RESTART) != 0) {
            ret = i2c_gpio_start(cfg, active);
            if (ret != 0) break;
            active = true;
            read = msg_read;
            ret = i2c_gpio_write_byte(cfg, (uint8_t)((address << 1) | read));
        } else if (msg_read != read) {
            ret = -EINVAL;
        }
        for (j = 0; j < messages[i].len && ret == 0; j++) {
            if (read) {
                bool last = j + 1 == messages[i].len &&
                    (i + 1 == count || (messages[i].flags & I2C_MSG_STOP) != 0 ||
                     (messages[i + 1].flags & I2C_MSG_RESTART) != 0 ||
                     (messages[i + 1].flags & I2C_MSG_READ) == 0);
                ret = i2c_gpio_read_byte(cfg, &messages[i].buf[j], last);
            } else {
                ret = i2c_gpio_write_byte(cfg, messages[i].buf[j]);
            }
        }
        if (ret == 0 && (messages[i].flags & I2C_MSG_STOP) != 0) {
            ret = i2c_gpio_stop(cfg);
            active = false;
        }
    }
    if (active) {
        int stop_ret = i2c_gpio_stop(cfg);
        if (ret == 0) ret = stop_ret;
    } else if (ret != 0) {
        cfg->ops->set_sda(cfg->context, true);
        cfg->ops->set_scl(cfg->context, true);
    }
    mutex_unlock(&controller->mutex);
    return ret;
}

const struct i2c_driver_api i2c_gpio_driver_api = { .transfer = i2c_gpio_transfer };

int i2c_gpio_init(struct device *dev)
{
    const struct i2c_gpio_config *cfg;
    int ret;

    if (dev == NULL || dev->config == NULL) return -EINVAL;
    cfg = dev->config;
    if (cfg->ops == NULL || cfg->ops->set_scl == NULL || cfg->ops->set_sda == NULL ||
        cfg->ops->get_scl == NULL || cfg->ops->get_sda == NULL || cfg->ops->delay == NULL ||
        cfg->stretch_limit == 0) return -EINVAL;
    ret = device_init(dev);
    if (ret != 0) return ret;
    cfg->ops->set_sda(cfg->context, true);
    cfg->ops->set_scl(cfg->context, true);
    ret = device_register(dev);
    if (ret != 0) return ret;
    dev->ready = 1;
    return 0;
}
