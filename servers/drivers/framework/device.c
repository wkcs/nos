/* Copyright (C) 2026 NOS contributors. */
#include "driver/device.h"

static int equal(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
int nos_driver_status(int status)
{
    return status > 0 ? -NOS_DRV_EIO : status;
}
struct nos_device *nos_device_find(const struct nos_device_registry *r, const char *name)
{
    size_t i;
    if (!r || !r->entries || r->count > r->capacity || !name)
        return NULL;
    for (i = 0; i < r->count; i++)
        if (r->entries[i] && equal(r->entries[i]->name, name))
            return r->entries[i];
    return NULL;
}
int nos_device_register(struct nos_device_registry *r, struct nos_device *d)
{
    size_t i, j;
    if (!r || !r->entries || r->count > r->capacity || !d || !d->name || !*d->name ||
        !d->compatible || !*d->compatible || d->state != NOS_DEVICE_NEW || d->driver || d->users ||
        d->leases || d->lease_count || d->resource_count > NOS_DRIVER_MAX_RESOURCES ||
        (d->resource_count && !d->resources))
        return -NOS_DRV_EINVAL;
    if (nos_device_find(r, d->name))
        return -NOS_DRV_EBUSY;
    if (d->parent) {
        for (i = 0; i < r->count && r->entries[i] != d->parent; i++)
            ;
        if (i == r->count)
            return -NOS_DRV_ENODEV;
    }
    for (i = 0; i < d->resource_count; i++) {
        const struct nos_resource *s = &d->resources[i];
        if ((unsigned)s->type > NOS_RESOURCE_CONTROLLER ||
            (s->type == NOS_RESOURCE_MMIO && (!s->size || s->start > UINT64_MAX - s->size)))
            return -NOS_DRV_EINVAL;
        for (j = 0; j < i; j++)
            if (d->resources[j].type == s->type && d->resources[j].id == s->id)
                return -NOS_DRV_EINVAL;
    }
    if (r->count == r->capacity)
        return -NOS_DRV_ENOSPC;
    r->entries[r->count++] = d;
    d->state = NOS_DEVICE_REGISTERED;
    return 0;
}
int nos_device_unregister(struct nos_device_registry *r, struct nos_device *d)
{
    size_t i, index;
    if (!r || !r->entries || r->count > r->capacity || !d)
        return -NOS_DRV_EINVAL;
    for (index = 0; index < r->count && r->entries[index] != d; index++)
        ;
    if (index == r->count)
        return -NOS_DRV_ENODEV;
    if (d->driver || d->users ||
        (d->state != NOS_DEVICE_REGISTERED && d->state != NOS_DEVICE_FAILED))
        return -NOS_DRV_EBUSY;
    for (i = 0; i < r->count; i++)
        if (r->entries[i]->parent == d)
            return -NOS_DRV_EBUSY;
    for (i = index + 1; i < r->count; i++)
        r->entries[i - 1] = r->entries[i];
    r->entries[--r->count] = NULL;
    d->state = NOS_DEVICE_NEW;
    return 0;
}
const struct nos_resource *nos_device_resource(const struct nos_device *d,
                                               enum nos_resource_type type, uint32_t id)
{
    unsigned i;
    if (!d || !d->resources || d->resource_count > NOS_DRIVER_MAX_RESOURCES)
        return NULL;
    for (i = 0; i < d->resource_count; i++)
        if (d->resources[i].type == type && d->resources[i].id == id)
            return &d->resources[i];
    return NULL;
}
int nos_device_acquire_resource(struct nos_device *d, enum nos_resource_type type, uint32_t id,
                                const struct nos_resource_provider *provider,
                                struct nos_resource_lease *lease)
{
    const struct nos_resource *resource;
    struct nos_resource_lease *l;
    uint64_t token = 0;
    int error;
    if (!d || !lease || lease->owner || lease->next || !provider || !provider->acquire ||
        !provider->release || !d->driver ||
        (d->state != NOS_DEVICE_PROBING && d->state != NOS_DEVICE_ACTIVE))
        return -NOS_DRV_EINVAL;
    resource = nos_device_resource(d, type, id);
    if (!resource)
        return -NOS_DRV_ENODEV;
    if (d->lease_count == NOS_DRIVER_MAX_RESOURCES)
        return -NOS_DRV_ENOSPC;
    for (l = d->leases; l; l = l->next)
        if (l->resource == resource)
            return -NOS_DRV_EBUSY;
    /* The provider must roll back its own failed acquisition. */
    error = nos_driver_status(provider->acquire(provider->context, resource, &token));
    if (error)
        return error;
    *lease = (struct nos_resource_lease){d->leases, d, resource, provider, token};
    d->leases = lease;
    d->lease_count++;
    return 0;
}
static void release_resources(struct nos_device *d)
{
    while (d->leases) {
        struct nos_resource_lease *lease = d->leases;
        d->leases = lease->next;
        lease->provider->release(lease->provider->context, lease->resource, lease->token);
        *lease = (struct nos_resource_lease){0};
    }
    d->lease_count = 0;
}
int nos_device_get(struct nos_device *d)
{
    if (!d || d->state != NOS_DEVICE_ACTIVE)
        return -NOS_DRV_ENODEV;
    if (d->users == UINT16_MAX)
        return -NOS_DRV_EBUSY;
    d->users++;
    return 0;
}
void nos_device_put(struct nos_device *d)
{
    if (d && d->users)
        d->users--;
}
int nos_device_bind(struct nos_device *d, const struct nos_driver *driver)
{
    int error;
    if (!d || !driver || !driver->probe || !driver->remove ||
        !equal(d->compatible, driver->compatible))
        return -NOS_DRV_EINVAL;
    if (d->driver || (d->state != NOS_DEVICE_REGISTERED && d->state != NOS_DEVICE_FAILED))
        return -NOS_DRV_EBUSY;
    if (d->parent && (error = nos_device_get(d->parent)))
        return error;
    d->driver = driver;
    d->state = NOS_DEVICE_PROBING;
    error = nos_driver_status(driver->probe(d));
    if (error) {
        driver->remove(d);
        release_resources(d);
        d->driver = NULL;
        d->state = NOS_DEVICE_FAILED;
        nos_device_put(d->parent);
    } else
        d->state = NOS_DEVICE_ACTIVE;
    return error;
}
int nos_device_unbind(struct nos_device *d)
{
    if (!d)
        return -NOS_DRV_EINVAL;
    if (!d->driver || (d->state != NOS_DEVICE_ACTIVE && d->state != NOS_DEVICE_SUSPENDED))
        return -NOS_DRV_ENODEV;
    if (d->users)
        return -NOS_DRV_EBUSY;
    d->state = NOS_DEVICE_PROBING;
    d->driver->remove(d);
    release_resources(d);
    d->driver = NULL;
    d->state = NOS_DEVICE_REGISTERED;
    nos_device_put(d->parent);
    return 0;
}
int nos_device_suspend(struct nos_device *d)
{
    int error;
    if (!d || d->state != NOS_DEVICE_ACTIVE || !d->driver)
        return -NOS_DRV_ENODEV;
    if (d->users)
        return -NOS_DRV_EBUSY;
    if (!d->driver->suspend || !d->driver->resume)
        return -NOS_DRV_ENOTSUP;
    d->state = NOS_DEVICE_PROBING;
    error = nos_driver_status(d->driver->suspend(d));
    d->state = error ? NOS_DEVICE_ACTIVE : NOS_DEVICE_SUSPENDED;
    return error;
}
int nos_device_resume(struct nos_device *d)
{
    int error;
    if (!d || d->state != NOS_DEVICE_SUSPENDED || !d->driver)
        return -NOS_DRV_ENODEV;
    if (!d->driver->resume)
        return -NOS_DRV_ENOTSUP;
    d->state = NOS_DEVICE_PROBING;
    error = nos_driver_status(d->driver->resume(d));
    d->state = error ? NOS_DEVICE_SUSPENDED : NOS_DEVICE_ACTIVE;
    return error;
}
