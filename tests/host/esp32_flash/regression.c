/* Copyright (C) 2026 NOS contributors. Actual NOR adapter, simulated SDK. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "flash_block.h"
#include "esp_err.h"
#define ERASE 4096u
static unsigned char flash[224 * ERASE], before[sizeof(flash)];
static esp_partition_t partition = {1, 0x40, 0x320000, sizeof(flash), false, false};
static unsigned missing, reads, erases, writes, fail_read, fail_erase, fail_write;
int idf_error(int error) {
    return error ? -NOS_DRV_EIO : 0;
}
const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype,
                                                const char *label) {
    assert(type == 1 && subtype == 0x40 && !strcmp(label, "storage"));
    return missing ? NULL : &partition;
}
int esp_partition_read(const esp_partition_t *p, size_t offset, void *data, size_t size) {
    assert(p == &partition && offset <= sizeof(flash) && size <= sizeof(flash) - offset);
    if (++reads == fail_read)
        return ESP_FAIL;
    memcpy(data, flash + offset, size);
    return 0;
}
int esp_partition_write(const esp_partition_t *p, size_t offset, const void *data, size_t size) {
    assert(p == &partition && offset <= sizeof(flash) && size <= sizeof(flash) - offset);
    unsigned torn = ++writes == fail_write;
    const unsigned char *bytes = data;
    if (torn && size > 13)
        size = 13;
    for (size_t i = 0; i < size; i++) {
        assert((flash[offset + i] & bytes[i]) == bytes[i]);
        flash[offset + i] &= bytes[i];
    }
    return torn ? ESP_FAIL : 0;
}
int esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size) {
    assert(p == &partition && !(offset % ERASE) && size == ERASE && offset < sizeof(flash));
    if (++erases == fail_erase)
        return ESP_FAIL;
    memset(flash + offset, 0xff, size);
    return 0;
}
int main(void) {
    struct nos_flash_block block = {0};
    missing = 1;
    assert(nos_flash_block_start(&block) == -NOS_DRV_ENODEV && !block.partition);
    missing = 0;
    partition.encrypted = true;
    assert(nos_flash_block_start(&block) == -NOS_DRV_ENOTSUP && !block.partition);
    partition.encrypted = false;
    partition.readonly = true;
    assert(nos_flash_block_start(&block) == -NOS_DRV_ENOTSUP && !block.partition);
    partition.readonly = false;
    partition.size = 65 * ERASE;
    assert(nos_flash_block_start(&block) == -NOS_DRV_EINVAL);
    partition.size = sizeof(flash);
    partition.address++;
    assert(nos_flash_block_start(&block) == -NOS_DRV_EINVAL);
    partition.address--;
    partition.subtype = 0x82; /* Existing SPIFFS must not be written. */
    assert(nos_flash_block_start(&block) == -NOS_DRV_EINVAL);
    partition.subtype = 0x40;
    memset(flash, 0xff, sizeof(flash));
    assert(!nos_flash_block_start(&block) && !reads && !erases && !writes);
    assert(nos_flash_block_start(&block) == -NOS_DRV_EBUSY);
    struct nos_io_block_info info;
    assert(!nos_flash_block_call(&block, NOS_IO_STATUS, &info, sizeof(info)));
    assert(info.sectors == 224 && info.sector_size == 512);
    unsigned char data[1024], result[1024];
    struct nos_io_block b = {.data = result, .count = 2, .sector = 32};
    assert(!nos_flash_block_call(&block, NOS_IO_READ, &b, sizeof(b)) && b.actual == 2);
    for (unsigned i = 0; i < sizeof(result); i++)
        assert(!result[i]);
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = i * 17u;
    b.data = data;
    assert(!nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) && b.actual == 2);
    b.data = result;
    assert(!nos_flash_block_call(&block, NOS_IO_READ, &b, sizeof(b)) &&
           !memcmp(result, data, sizeof(data)));
    puts("PASS flash_partition_guards_geometry_erased_zero_binary_roundtrip");
    memcpy(before, flash, sizeof(flash));
    b.sector = UINT64_MAX;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EINVAL &&
           !b.actual);
    b.sector = 223;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EINVAL);
    b.sector = 0;
    b.count = NOS_DRIVER_MAX_TRANSFER / 512 + 1;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EINVAL);
    b.count = 0;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EINVAL);
    assert(nos_flash_block_call(&block, NOS_IO_ERASE, NULL, 0) == -NOS_DRV_ENOTSUP);
    assert(!memcmp(before, flash, sizeof(flash)));
    b = (struct nos_io_block){.data = data, .count = 2, .sector = 32};
    fail_erase = erases + 1;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EIO && !b.actual &&
           !memcmp(before, flash, sizeof(flash)));
    fail_erase = 0;
    fail_write = writes + 2;
    assert(nos_flash_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EIO &&
           b.actual == 1);
    /* Writing logical sectors 32/33 cannot erase either neighbour, even at
     * the native filesystem's non-4K-aligned bank boundary. */
    assert(!memcmp(before, flash, 32 * ERASE));
    assert(!memcmp(before + 34 * ERASE, flash + 34 * ERASE, sizeof(flash) - 34 * ERASE));
    fail_read = reads + 1;
    b.data = result;
    assert(nos_flash_block_call(&block, NOS_IO_READ, &b, sizeof(b)) == -NOS_DRV_EIO && !b.actual);
    assert(!nos_flash_block_call(&block, NOS_IO_COMMIT, NULL, 0));
    assert(!nos_flash_block_stop(&block));
    assert(nos_flash_block_call(&block, NOS_IO_READ, &b, sizeof(b)) == -NOS_DRV_ENODEV);
    puts("PASS flash_bounds_sdk_faults_partial_progress_independent_erase_blocks");
}
