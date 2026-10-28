/** Copyright (C) 2026 Nick Hu */
#include <nos/driver.h>
#include <kernel/errno.h>

int nos_dt_device_bind(struct device *dev, const struct nos_dt_node *node,
                       const struct nos_dt_driver *driver)
{
    const struct nos_dt_match *match = NULL;
    size_t i;
    int ret;

    if (dev == NULL || node == NULL || driver == NULL || driver->probe == NULL ||
        driver->matches == NULL || driver->match_count == 0) {
        return -EINVAL;
    }
    if (!nos_dt_is_available(node)) {
        return -ENODEV;
    }
    for (i = 0; i < driver->match_count; i++) {
        if (driver->matches[i].compatible != NULL &&
            nos_dt_is_compatible(node, driver->matches[i].compatible)) {
            match = &driver->matches[i];
            break;
        }
    }
    if (match == NULL) {
        return -ENODEV;
    }
    mutex_lock(&dev->mutex);
    if (dev->dt_driver != NULL) {
        mutex_unlock(&dev->mutex);
        return -EALREADY;
    }
    if (dev->dt_binding) {
        mutex_unlock(&dev->mutex);
        return -EBUSY;
    }
    dev->dt_binding = 1;
    mutex_unlock(&dev->mutex);

    ret = driver->probe(dev, node, match->data);

    mutex_lock(&dev->mutex);
    if (ret == 0) {
        dev->dt_driver = driver;
        dev->dt_node = node;
    }
    dev->dt_binding = 0;
    mutex_unlock(&dev->mutex);
    return ret;
}
