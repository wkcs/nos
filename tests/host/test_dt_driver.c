/** Copyright (C) 2026 Nick Hu */
#include <nos/driver.h>
#include <kernel/errno.h>
#include <stdio.h>

static const uint8_t compatible[] = "vendor,chip\0vendor,generic";
static const uint8_t disabled[] = "disabled";
static const struct nos_dt_property props[] = { { "compatible", compatible, sizeof(compatible) } };
static const struct nos_dt_property disabled_props[] = {
    { "compatible", compatible, sizeof(compatible) }, { "status", disabled, sizeof(disabled) },
};
const struct nos_dt_node nos_dt_nodes[] = {
    { "/", NULL, 0, -1 }, { "/chip", props, 1, 0 },
    { "/disabled", disabled_props, 2, 0 },
};
const size_t nos_dt_node_count = 3;
static int calls, should_defer = 1;
static const struct nos_dt_driver *nested_driver;
void mutex_lock(struct mutex *lock) { if (lock->locked) calls = -100; lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { if (!lock->locked) calls = -100; lock->locked = 0; }
static int probe(struct device *dev, const struct nos_dt_node *node, const void *data)
{
    if (dev->mutex.locked || node != &nos_dt_nodes[1] || data != &calls) return -EINVAL;
    calls++;
    if (nos_dt_device_bind(dev, node, nested_driver) != -EBUSY) return -EINVAL;
    return should_defer ? -EAGAIN : 0;
}

int main(void)
{
    const struct nos_dt_match matches[] = { { "vendor,chip", &calls } };
    const struct nos_dt_driver driver = { .name = "chip", .matches = matches,
                                           .match_count = 1, .probe = probe };
    struct device dev = { .ready = true };
    nested_driver = &driver;
    if (nos_dt_device_bind(&dev, &nos_dt_nodes[2], &driver) != -ENODEV || calls != 0 ||
        nos_dt_device_bind(&dev, &nos_dt_nodes[1], &driver) != -EAGAIN || calls != 1 ||
        dev.dt_driver != NULL || dev.mutex.locked) {
        fputs("disabled node or deferred probe handling failed\n", stderr); return 1;
    }
    should_defer = 0;
    if (nos_dt_device_bind(&dev, &nos_dt_nodes[1], &driver) != 0 || calls != 2 ||
        dev.dt_driver != &driver || dev.dt_node != &nos_dt_nodes[1] || dev.mutex.locked ||
        nos_dt_device_bind(&dev, &nos_dt_nodes[1], &driver) != -EALREADY || calls != 2) {
        fputs("device binding or duplicate bind handling failed\n", stderr); return 2;
    }
    puts("device-tree driver binding checks passed");
    return 0;
}
