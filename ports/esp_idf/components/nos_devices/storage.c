/* Copyright (C) 2026 NOS contributors. SDK storage behind NOS services. */
#include <string.h>
#include "private.h"
#include "nvs_flash.h"
static unsigned nvs_users; /* Serialized by the common NOS service domain. */
int idf_nvs_acquire(void)
{
    int rc = nvs_users ? 0 : idf_error(nvs_flash_init());
    /* Never silently erase NVS on an initialization error. */
    if (!rc)
        nvs_users++;
    return rc;
}
int idf_nvs_release(void)
{
    if (!nvs_users)
        return -NOS_DRV_EINVAL;
    int rc = nvs_users == 1 ? idf_error(nvs_flash_deinit()) : 0;
    if (!rc)
        nvs_users--;
    return rc;
}
static int storage_start(void *context)
{
    struct idf_device *d = context;
    if (!d->config.storage_namespace || !*d->config.storage_namespace ||
        strlen(d->config.storage_namespace) > 15)
        return -NOS_DRV_EINVAL;
    int rc = idf_nvs_acquire();
    if (rc)
        return rc;
    d->nvs_owned = 1;
    rc = idf_error(nvs_open(d->config.storage_namespace, NVS_READWRITE, &d->u.kv.handle));
    if (!rc)
        d->installed = 1;
    return rc;
}
static int storage_stop(void *context)
{
    struct idf_device *d = context;
    if (d->installed) {
        nvs_close(d->u.kv.handle);
        d->installed = 0;
    }
    int rc = d->nvs_owned ? idf_nvs_release() : 0;
    if (!rc)
        d->nvs_owned = 0;
    return rc;
}
static int storage_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_COMMIT && !size)
        return idf_error(nvs_commit(d->u.kv.handle));
    if (op == NOS_IO_ERASE && !size)
        return idf_error(nvs_erase_all(d->u.kv.handle));
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_kv)) {
        struct nos_io_kv *kv = request;
        if (!kv->key || !*kv->key || strlen(kv->key) > 15)
            return -NOS_DRV_EINVAL;
        return idf_error(op == NOS_IO_READ ? nvs_get_u32(d->u.kv.handle, kv->key, &kv->value)
                                           : nvs_set_u32(d->u.kv.handle, kv->key, kv->value));
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_storage_ops = {storage_start, storage_stop, storage_call};

static int ota_start(void *context)
{
    (void)context;
    return 0;
}
static int ota_stop(void *context)
{
    struct idf_device *d = context;
    int rc = d->u.ota.begun ? idf_error(esp_ota_abort(d->u.ota.handle)) : 0;
    if (!rc)
        d->u.ota.begun = 0;
    return rc;
}
static int ota_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_OTA_BEGIN && size == sizeof(struct nos_io_ota_begin)) {
        const esp_partition_t *active = esp_ota_get_running_partition();
        const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
        struct nos_io_ota_begin *begin = request;
        if (d->u.ota.begun)
            return -NOS_DRV_EBUSY;
        if (!active || !inactive || active->address == inactive->address)
            return -NOS_DRV_ENODEV;
        if (!begin->image_size || begin->image_size > inactive->size)
            return -NOS_DRV_EINVAL;
        int rc = idf_error(esp_ota_begin(inactive, begin->image_size, &d->u.ota.handle));
        if (!rc)
            d->u.ota.begun = 1;
        return rc;
    }
    if (!d->u.ota.begun)
        return -NOS_DRV_ENODEV;
    if (op == NOS_IO_WRITE && size == sizeof(struct nos_io_buffer)) {
        struct nos_io_buffer *b = request;
        b->actual = 0;
        if (!b->data || !b->size || b->size > NOS_DRIVER_MAX_TRANSFER)
            return -NOS_DRV_EINVAL;
        int rc = idf_error(esp_ota_write(d->u.ota.handle, b->data, b->size));
        if (!rc)
            b->actual = b->size;
        return rc;
    }
    if (op == NOS_IO_OTA_FINISH && !size) {
        /* IDF consumes the handle even on validation failure. */
        int rc = idf_error(esp_ota_end(d->u.ota.handle));
        d->u.ota.begun = 0;
        return rc;
    }
    if (op == NOS_IO_OTA_ABORT && !size)
        return ota_stop(d);
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_ota_ops = {ota_start, ota_stop, ota_call};
