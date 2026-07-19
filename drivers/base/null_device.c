/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <kernel/device.h>
#include <kernel/printk.h>

static int null_init(struct device *dev)
{
    (void)dev;

    return 0;
}

DEVICE_DEFINE(nulldev, "null", null_init, NULL, NULL, NULL, POST_KERNEL);

static int null_device_verify(void)
{
    struct device *dev;

    dev = device_get_binding("null");
    if (device_is_ready(dev)) {
        pr_info("device null ready\r\n");
        return 0;
    }

    pr_err("device null not ready\r\n");
    return -ENODEV;
}

SYS_INIT(null_device_verify, INIT_LEVEL_APPLICATION);
