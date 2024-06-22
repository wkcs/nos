/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[DEVICE]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/device.h>
#include <kernel/printk.h>
#include <string.h>

static LIST_HEAD(g_sys_device_list);
static SPINLOCK(g_sys_device_list_lock);

int device_init(struct device *dev)
{
    if (dev == NULL) {
        pr_err("dev is NULL\r\n");
        return -EINVAL;
    }

    INIT_LIST_HEAD(&dev->list);
    dev->ref_count = 0;
    mutex_init(&dev->mutex);

    return 0;
}

void device_put(struct device *dev)
{
    if (dev->ref_count > 0)
        dev->ref_count--;
}

void device_inc(struct device *dev)
{
    if (dev->ref_count < U32_MAX)
        dev->ref_count++;
}

int device_register(struct device *dev)
{
    struct device *dev_tmp;

    if (dev == NULL) {
        pr_err("dev is NULL\r\n");
        return -EINVAL;
    }

    dev_tmp = device_find_by_name(dev->name);
    if (dev_tmp != NULL) {
        pr_err("device \"%s\" already exists\r\n", dev->name);
        return -EALREADY;
    }

    spin_lock(&g_sys_device_list_lock);
    list_add(&dev->list, &g_sys_device_list);
    spin_unlock(&g_sys_device_list_lock);

    return 0;
}

int device_unregister(struct device *dev)
{
    if (dev == NULL) {
        pr_err("dev is NULL\r\n");
        return -EINVAL;
    }

    spin_lock(&g_sys_device_list_lock);
    list_del(&dev->list);
    spin_unlock(&g_sys_device_list_lock);

    return 0;
}

struct device *device_find_by_name(const char *name)
{
    struct device *dev;

    if (name == NULL) {
        pr_err("name is NULL\r\n");
        return NULL;
    }

    spin_lock(&g_sys_device_list_lock);
    list_for_each_entry(dev, &g_sys_device_list, list) {
        if (!strcmp(name, dev->name)) {
            spin_unlock(&g_sys_device_list_lock);
            return dev;
        }
    }
    spin_unlock(&g_sys_device_list_lock);

    return NULL;
}
