/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_DEVICE_H__
#define __NOS_DEVICE_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/mutex.h>

struct device;

struct device_ops {
    int (*init)(struct device *dev);
    int (*open)(struct device *dev, uint16_t oflag);
    int (*close)(struct device *dev);
    ssize_t (*read)(struct device *dev, addr_t pos,
        void *buffer, size_t size);
    ssize_t (*write)(struct device *dev, addr_t pos,
        const void *buffer, size_t size);
    ssize_t (*read_timeout)(struct device *dev, addr_t pos,
        void *buffer, size_t size, uint32_t tick);
    ssize_t (*write_timeout)(struct device *dev, addr_t pos,
        const void *buffer, size_t size, uint32_t tick);
    int (*control)(struct device *dev, int cmd, void *args);
};

struct device {
    const char *name;
    void *priv;
    struct device_ops ops;
    struct mutex mutex;
    struct list_head list;
    uint32_t ref_count;
};

int device_init(struct device *dev);
void device_put(struct device *dev);
void device_inc(struct device *dev);
int device_register(struct device *dev);
int device_unregister(struct device *dev);
struct device *device_find_by_name(const char *name);

#endif /* __NOS_DEVICE_H__ */