/* Copyright (C) 2026 NOS contributors. NOS owns the storage controller lease. */
#include "private.h"
#ifdef NOS_IDF_STORAGE_WL
#define block_start nos_wl_block_start
#define block_stop nos_wl_block_stop
#define block_call nos_wl_block_call
#else
#define block_start nos_flash_block_start
#define block_stop nos_flash_block_stop
#define block_call nos_flash_block_call
#endif
const char *nos_idf_storage_format(void) {
#ifdef NOS_IDF_STORAGE_WL
    return "wl";
#else
    return "raw";
#endif
}
static int flash_start(void *context) {
    struct idf_device *d = context;
    return block_start(&d->u.block);
}
static int flash_stop(void *context) {
    struct idf_device *d = context;
    return block_stop(&d->u.block);
}
static int flash_call(void *context, uint32_t command, void *request, size_t size) {
    struct idf_device *d = context;
    return block_call(&d->u.block, command, request, size);
}
const struct nos_service_ops idf_flash_ops = {flash_start, flash_stop, flash_call};
