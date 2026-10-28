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
#include <kernel/init.h>
#include <kernel/list.h>
#include <kernel/mutex.h>

struct device;
struct nos_dt_driver;
struct nos_dt_node;

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
    const void *config;
    union {
        void *data;
        void *priv;
    };
    const void *api;
    const struct nos_dt_driver *dt_driver;
    const struct nos_dt_node *dt_node;
    uint8_t dt_binding;
    uint8_t ready;
    int init_res;
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
struct device *device_get_binding(const char *name);
int device_is_ready(const struct device *dev);

/*
 * Define a static device and initialize it at the requested SYS_INIT level.
 * The init callback receives the device so it can use its config, data, and
 * api fields.  A device becomes ready only after both initialization and
 * registration succeed.
 */
#define DEVICE_DEFINE(dev_id, dev_name, init_fn, cfg_ptr, data_ptr, api_ptr, level) \
    static struct device __device_##dev_id = { \
        .name = (dev_name), \
        .config = (cfg_ptr), \
        .data = (data_ptr), \
        .api = (api_ptr), \
    }; \
    static int __device_init_##dev_id(void) \
    { \
        int ret; \
        \
        ret = device_init(&__device_##dev_id); \
        if (ret == 0) { \
            ret = init_fn(&__device_##dev_id); \
        } \
        if (ret == 0) { \
            ret = device_register(&__device_##dev_id); \
        } \
        __device_##dev_id.init_res = ret; \
        __device_##dev_id.ready = (ret == 0); \
        return ret; \
    } \
    SYS_INIT(__device_init_##dev_id, INIT_LEVEL_##level)

#endif /* __NOS_DEVICE_H__ */
