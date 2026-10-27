/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_DEVICE_H
#define NOS_TEST_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
struct device {
    const void *api;
    const void *config;
    void *data;
    bool ready;
};
static inline int device_is_ready(const struct device *dev)
{
    return dev != NULL && dev->ready;
}
#define DEVICE_DEFINE(id, name, init, cfg, instance, driver_api, level) \
    static int (*__device_init_fn_##id)(struct device *)                \
        __attribute__((used)) = init;                                   \
    static struct device __device_##id __attribute__((used)) = {        \
        .config = cfg, .data = instance, .api = driver_api, .ready = true \
    }
#endif
