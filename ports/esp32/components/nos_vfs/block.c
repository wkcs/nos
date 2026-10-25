/* Copyright (C) 2026 NOS contributors. Native VFS over the managed NOR service. */
#include "nos/driver/esp_idf.h"
#include "../../../../servers/drivers/devices.h"
/* Only the single VFS task calls this adapter. The device domain serializes
 * physical I/O with other hardware operations; no SDK handle leaves it. */
static struct nos_service *storage;
static struct nos_io_block_info geometry;
static uint64_t requests;
static unsigned ready;

int nos_block_init(void) {
    if (storage)
        return ready ? 0 : -NOS_EIO;
    const struct nos_idf_device_config config = {.name = "vfs.storage", .kind = NOS_SERVICE_BLOCK};
    int rc = nos_idf_device_create(&config, &storage);
    if (!rc)
        rc = nos_service_start(storage);
    if (!rc)
        rc = nos_service_call(storage, NOS_IO_STATUS, &geometry, sizeof(geometry));
    if (!rc && (geometry.sector_size != 512 || geometry.sectors < 66))
        rc = -NOS_ENOTSUP;
    if (!rc)
        ready = 1;
    if (rc)
        geometry = (struct nos_io_block_info){0};
    if (rc && storage) {
        int closed = nos_idf_device_destroy(&storage);
        if (closed)
            return closed;
    }
    return rc;
}
static int transfer(unsigned sector, void *buffer, uint32_t command) {
    if (!storage)
        return -NOS_DRV_ENODEV;
    struct nos_io_block b = {.sector = sector, .count = 1, .data = buffer};
    requests++;
    int rc = nos_service_call(storage, command, &b, sizeof(b));
    return rc ? rc : b.actual == 1 ? 0 : -NOS_EIO;
}
int nos_block_read(unsigned sector, void *buffer) {
    return transfer(sector, buffer, NOS_IO_READ);
}
int nos_block_write(unsigned sector, const void *buffer) {
    return transfer(sector, (void *)buffer, NOS_IO_WRITE);
}
int nos_block_flush(void) {
    return storage ? nos_service_call(storage, NOS_IO_COMMIT, NULL, 0) : -NOS_DRV_ENODEV;
}
uint64_t nos_block_capacity(void) {
    return geometry.sectors;
}
uint64_t nos_block_interrupts(void) {
    return 0; /* HAL-internal IRQ activity is not observable here. */
}
uint64_t nos_block_requests(void) {
    return requests;
}
uint64_t nos_block_poll_fallbacks(void) {
    return 0;
}
unsigned nos_block_version(void) {
    return storage ? 1 : 0;
}
