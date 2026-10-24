/* Copyright (C) 2026 NOS contributors. */
#include "abi.h"
#include "driver/peripheral.h"

/* Actual portable framework runs in each guest's user task. Injected resources
 * deliberately do not stand in for physical MMIO or kernel authorization. */
struct driver_fixture {
    struct nos_resource_lease lease;
    const struct nos_resource_provider *provider;
    unsigned owned, releases;
    int fail, level;
};
static int acquire(void *context, const struct nos_resource *resource, uint64_t *token)
{
    struct driver_fixture *f = context;
    f->owned++;
    *token = resource->start;
    return 0;
}
static void release(void *context, const struct nos_resource *resource, uint64_t token)
{
    struct driver_fixture *f = context;
    if (token == resource->start && f->owned) {
        f->owned--;
        f->releases++;
    }
}
static int probe(struct nos_device *d)
{
    struct driver_fixture *f = d->context;
    int error = nos_device_acquire_resource(d, NOS_RESOURCE_MMIO, 0, f->provider, &f->lease);
    return error ? error : f->fail ? -NOS_DRV_EIO : 0;
}
static void remove_device(struct nos_device *d)
{
    (void)d;
}
static const struct nos_driver driver = {"test,portable", probe, remove_device, NULL, NULL};
static int gpio_read(void *context, uint32_t pin, int *value)
{
    struct driver_fixture *f = context;
    if (pin != 2)
        return -NOS_DRV_EINVAL;
    *value = f->level;
    return 0;
}
static int gpio_write(void *context, uint32_t pin, int value)
{
    struct driver_fixture *f = context;
    if (pin != 2)
        return -NOS_DRV_EINVAL;
    f->level = value;
    return 0;
}
static const struct nos_gpio_ops gpio_ops = {NULL, gpio_read, gpio_write};
void nos_driver_regression(void)
{
    /* Explicit context indirection avoids requiring hosted TLS/global state. */
    struct fixture_with_provider {
        struct driver_fixture fixture;
        struct nos_resource_provider provider;
    } storage = {0};
    struct driver_fixture *f = &storage.fixture;
    const struct nos_resource resource = {UINT64_C(0x123456780000), 4096, 0, NOS_RESOURCE_MMIO};
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    struct nos_device device = {.name = "portable",
                                .compatible = "test,portable",
                                .context = f,
                                .resources = &resource,
                                .resource_count = 1};
    struct nos_gpio_controller gc = {{&device, f, 1, 0}, &gpio_ops, 8};
    struct nos_gpio pin = {&gc, 2, 1};
    int value = -1;
    int ok;
    storage.provider = (struct nos_resource_provider){f, acquire, release};
    f->provider = &storage.provider;
    f->fail = 1;
    ok = !nos_device_register(&registry, &device);
    ok &= nos_device_bind(&device, &driver) == -NOS_DRV_EIO;
    ok &= device.state == NOS_DEVICE_FAILED && !f->owned && f->releases == 1;
    f->fail = 0;
    ok &= !nos_device_bind(&device, &driver) && f->owned == 1;
    ok &= !nos_gpio_write(&pin, 1) && f->level == 0;
    ok &= !nos_gpio_read(&pin, &value) && value == 1;
    ok &= !nos_device_get(&device);
    ok &= nos_device_unbind(&device) == -NOS_DRV_EBUSY;
    nos_device_put(&device);
    ok &= !nos_device_unbind(&device) && !f->owned && f->releases == 2;
    ok &= nos_gpio_read(&pin, &value) == -NOS_DRV_ENODEV;
    ok &= !nos_device_unregister(&registry, &device);
    nos_check(ok, "driver_framework_userspace_lifecycle");
}
