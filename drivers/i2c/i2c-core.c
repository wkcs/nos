#include <wk/device.h>
#include <wk/mm.h>
#include <wk/list.h>
#include <wk/err.h>
#include <string.h>


int i2c_bus_match(struct device *dev, struct device_driver *drv)
{
    if (strcmp(dev->name, drv->of_match_table->name) == 0)
        return 0;

    pr_info("match err\r\n");
    return -EINVAL;
}

static struct bus_type i2c_bus = {
    .name = "i2c",
    .match = i2c_bus_match,
};

static struct device i2c_device = {
    .name = "i2c-dev",
    .bus = &i2c_bus,
};

int i2c_core_init(void)
{
    int ret;

    ret = bus_register(&i2c_bus);
    if (ret != 0) {
        pr_err("i2c_bus register err\r\n");
        return ret;
    }

    ret = dev_register(&i2c_device);
    if (ret != 0) {
        pr_err("i2c device register err\r\n");
        return ret;
    }

    return 0;
}

int i2c_probe(__maybe_unused struct device *dev)
{
    pr_info("i2c probe success\r\n");
    return 0;
}

int i2c_remove(__maybe_unused struct device *dev)
{
    pr_info("i2c remove success\r\n");
    return 0;
}

static struct of_device_id i2c_match_table = {
    .name = "i2c-dev",
};

static struct device_driver i2c_driver = {
    .name = "i2c-driver",
    .bus = &i2c_bus,
    .of_match_table = &i2c_match_table,
    .probe = i2c_probe,
    .remove = i2c_remove,
};

int __i2c_init(void)
{
    int ret;

    ret = drv_register(&i2c_driver);
    if (ret != 0) {
        pr_err("i2c driver register err\r\n");
        return ret;
    }

    return 0;
}

int __i2c_remove(void)
{
    drv_unregister(&i2c_driver);

    return 0;
}

