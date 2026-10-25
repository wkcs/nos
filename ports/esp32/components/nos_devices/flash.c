/* Copyright (C) 2026 NOS contributors. NOS owns the storage controller lease. */
#include "private.h"
static int flash_start(void *context) {
    struct idf_device *d = context;
    return nos_flash_block_start(&d->u.block);
}
static int flash_stop(void *context) {
    struct idf_device *d = context;
    return nos_flash_block_stop(&d->u.block);
}
static int flash_call(void *context, uint32_t command, void *request, size_t size) {
    struct idf_device *d = context;
    return nos_flash_block_call(&d->u.block, command, request, size);
}
const struct nos_service_ops idf_flash_ops = {flash_start, flash_stop, flash_call};
