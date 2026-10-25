/* Copyright (C) 2026 NOS contributors. Original SDK WL + actual NOS guard. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "flash_block.h"
#include "esp_err.h"
static std::vector<unsigned char> image(0xe0000, 0xff), saved;
static esp_partition_t part = {1, 0x41, 0x320000, 0xe0000, false, false, 4096};
static unsigned reads, writes, erases, fail_read, fail_write, fail_erase;
static unsigned counts[224];
extern "C" int idf_error(int rc) {
    return rc ? -NOS_DRV_EIO : 0;
}
uint32_t esp_random(void) {
    return 0x12345678;
}
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *bytes, size_t length) {
    crc = ~crc;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype,
                                                const char *label) {
    assert(type == 1 && subtype == 0x41 && !strcmp(label, "storage"));
    return &part;
}
int esp_partition_read(const esp_partition_t *p, size_t offset, void *buffer, size_t size) {
    assert(p == &part && offset <= image.size() && size <= image.size() - offset);
    if (++reads == fail_read)
        return ESP_FAIL;
    memcpy(buffer, image.data() + offset, size);
    return 0;
}
int esp_partition_write(const esp_partition_t *p, size_t offset, const void *buffer, size_t size) {
    assert(p == &part && offset <= image.size() && size <= image.size() - offset);
    bool torn = ++writes == fail_write;
    if (torn && size > 7)
        size = 7;
    auto *b = (const unsigned char *)buffer;
    for (size_t i = 0; i < size; i++) {
        assert((image[offset + i] & b[i]) == b[i]);
        image[offset + i] &= b[i];
    }
    return torn ? ESP_FAIL : 0;
}
int esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size) {
    assert(p == &part && !(offset % 4096) && !(size % 4096) && offset <= image.size() &&
           size <= image.size() - offset);
    if (++erases == fail_erase)
        return ESP_FAIL;
    for (size_t i = offset / 4096; i < (offset + size) / 4096; i++)
        counts[i]++;
    memset(image.data() + offset, 0xff, size);
    return 0;
}
int main(void) {
    nos_flash_block block = {};
    assert(!nos_wl_block_start(&block));
    nos_io_block_info info;
    assert(!nos_wl_block_call(&block, NOS_IO_STATUS, &info, sizeof(info)));
    assert(info.sectors == 220 && info.sector_size == 512);
    unsigned char data[512], result[512];
    nos_io_block b = {.data = data, .sector = 0, .count = 1, .actual = 0};
    for (unsigned i = 0; i < 600; i++) {
        memset(data, i, sizeof(data));
        assert(!nos_wl_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) && b.actual == 1);
        assert(!nos_wl_block_call(&block, NOS_IO_COMMIT, nullptr, 0));
    }
    unsigned distinct = 0;
    for (unsigned i = 0; i < 221; i++)
        distinct += counts[i] != 0;
    assert(distinct == 221);
    assert(!nos_wl_block_stop(&block));
    assert(!nos_wl_block_start(&block));
    b.data = result;
    assert(!nos_wl_block_call(&block, NOS_IO_READ, &b, sizeof(b)) &&
           !memcmp(data, result, sizeof(data)));
    assert(!nos_wl_block_stop(&block));
    saved = image;
    /* Corrupt either redundant state; original vendor recovery must retain data. */
    image[image.size() - 3 * 4096 + 60] ^= 1;
    assert(!nos_wl_block_start(&block));
    assert(!nos_wl_block_call(&block, NOS_IO_READ, &b, sizeof(b)) &&
           !memcmp(data, result, sizeof(data)));
    assert(!nos_wl_block_stop(&block));
    puts("PASS wl_original_algorithm_rotation_reopen_and_single_state_repair");
    image = saved;
    image[image.size() - 3 * 4096 + 60] ^= 1;
    image[image.size() - 2 * 4096 + 60] ^= 1;
    auto damaged = image;
    assert(nos_wl_block_start(&block) == -NOS_DRV_EIO && !block.wear && image == damaged);
    image.assign(image.size(), 0xff);
    image[42] = 0;
    damaged = image;
    assert(nos_wl_block_start(&block) == -NOS_DRV_EIO && image == damaged);
    image = saved;
    unsigned previous_writes = writes, previous_erases = erases;
    fail_read = reads + 1;
    assert(nos_wl_block_start(&block) == -NOS_DRV_EIO && writes == previous_writes &&
           erases == previous_erases);
    fail_read = 0;
    assert(!nos_wl_block_start(&block));
    fail_write = writes + 1;
    b.data = data;
    assert(nos_wl_block_call(&block, NOS_IO_WRITE, &b, sizeof(b)) == -NOS_DRV_EIO && !b.actual);
    previous_writes = writes;
    previous_erases = erases;
    b.actual = 99;
    assert(nos_wl_block_call(&block, NOS_IO_READ, &b, sizeof(b)) == -NOS_DRV_EIO && !b.actual);
    assert(nos_wl_block_call(&block, NOS_IO_COMMIT, nullptr, 0) == -NOS_DRV_EIO);
    assert(!nos_wl_block_stop(&block) && writes == previous_writes && erases == previous_erases);
    puts("PASS wl_corrupt_unknown_read_failure_reject_and_io_fault_poison_no_flush");
}
