/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_DEVICE_H
#define NOS_DRIVER_DEVICE_H
#include <stddef.h>
#include <stdint.h>
/* User-service API, not a syscall or a wire ABI. The owning event loop must
 * serialize lifecycle and I/O. Callbacks must not reenter lifecycle methods.
 * No allocation, global registry, implicit MMIO mapping or kernel privilege. */
#define NOS_DRV_EIO 5
#define NOS_DRV_EAGAIN 11
#define NOS_DRV_EBUSY 16
#define NOS_DRV_ENODEV 19
#define NOS_DRV_EINVAL 22
#define NOS_DRV_ENOSPC 28
#define NOS_DRV_ENOTSUP 95
#define NOS_DRV_ETIMEDOUT 110
#define NOS_DRIVER_MAX_RESOURCES 16u
#define NOS_DRIVER_MAX_MESSAGES 16u
#define NOS_DRIVER_MAX_TRANSFER 65536u

enum nos_device_state {
    NOS_DEVICE_NEW,
    NOS_DEVICE_REGISTERED,
    NOS_DEVICE_PROBING,
    NOS_DEVICE_ACTIVE,
    NOS_DEVICE_SUSPENDED,
    NOS_DEVICE_FAILED
};
enum nos_resource_type {
    NOS_RESOURCE_MMIO,
    NOS_RESOURCE_IRQ,
    NOS_RESOURCE_DMA,
    NOS_RESOURCE_CLOCK,
    NOS_RESOURCE_RESET,
    NOS_RESOURCE_PINCTRL
};
/* Descriptive only. id identifies a platform provider/capability; start is a
 * bus address, never a pointer. Driver services must obtain kernel authorization
 * before mapping MMIO, binding IRQ or programming DMA. */
struct nos_resource {
    uint64_t start, size;
    uint32_t id;
    enum nos_resource_type type;
};
struct nos_device;
struct nos_resource_lease;
/* Provider bridges service-owned resources to authorized platform/kernel
 * mechanisms. A successful acquire owns a token; release must quiesce DMA/IRQ
 * and invalidate mappings. Descriptors do not authorize anything by themselves. */
struct nos_resource_provider {
    void *context;
    int (*acquire)(void *, const struct nos_resource *, uint64_t *token);
    void (*release)(void *, const struct nos_resource *, uint64_t token);
};
/* Caller-owned, zero-initialized, immovable until device unbind/probe failure.
 * Resources release in reverse acquisition order after driver remove(). */
struct nos_resource_lease {
    struct nos_resource_lease *next;
    struct nos_device *owner;
    const struct nos_resource *resource;
    const struct nos_resource_provider *provider;
    uint64_t token;
};
struct nos_driver {
    const char *compatible;
    int (*probe)(struct nos_device *);
    /* Called on failed probe too. Must release partially acquired resources,
     * stop DMA/IRQ and tolerate an empty context. Cannot fail. */
    void (*remove)(struct nos_device *);
    int (*suspend)(struct nos_device *);
    int (*resume)(struct nos_device *);
};
struct nos_device {
    const char *name, *compatible;
    struct nos_device *parent;
    const struct nos_resource *resources;
    void *context;
    const struct nos_driver *driver;
    struct nos_resource_lease *leases;
    uint16_t users;
    uint8_t resource_count, state, lease_count;
};
/* Caller owns entries and all strings/descriptors until unregister. */
struct nos_device_registry {
    struct nos_device **entries;
    size_t capacity, count;
};
int nos_device_acquire_resource(struct nos_device *, enum nos_resource_type, uint32_t id,
                                const struct nos_resource_provider *, struct nos_resource_lease *);
int nos_device_register(struct nos_device_registry *, struct nos_device *);
int nos_device_unregister(struct nos_device_registry *, struct nos_device *);
struct nos_device *nos_device_find(const struct nos_device_registry *, const char *name);
const struct nos_resource *nos_device_resource(const struct nos_device *, enum nos_resource_type,
                                               uint32_t id);
int nos_device_bind(struct nos_device *, const struct nos_driver *);
int nos_device_unbind(struct nos_device *);
int nos_device_suspend(struct nos_device *);
int nos_device_resume(struct nos_device *);
int nos_device_get(struct nos_device *);
void nos_device_put(struct nos_device *);
/* Reject unexpected positive callback statuses rather than fake success. */
int nos_driver_status(int status);
#endif
