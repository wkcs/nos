/* Copyright (C) 2026 NOS contributors. Guarded original IDF 5.4.2 WL classes. */
#include <cstdlib>
#include <new>
#include "flash_block.h"
#include "WL_Flash.h"
#include "crc32.h"
extern "C" int idf_error(int);
static constexpr size_t erase_bytes = 4096, logical_bytes = 512;

/* The SDK initializer ignores its first read result. Latch every physical
 * error, preventing later writes or erases based on incomplete metadata. */
class guarded_partition final : public Partition {
  public:
    bool failed = false;
    explicit guarded_partition(const esp_partition_t *p) : Partition(p) {}
    esp_err_t read(size_t offset, void *data, size_t size) override {
        if (failed)
            return ESP_FAIL;
        esp_err_t rc = Partition::read(offset, data, size);
        failed |= rc != ESP_OK;
        return rc;
    }
    esp_err_t write(size_t offset, const void *data, size_t size) override {
        if (failed)
            return ESP_FAIL;
        esp_err_t rc = Partition::write(offset, data, size);
        failed |= rc != ESP_OK;
        return rc;
    }
    esp_err_t erase_range(size_t offset, size_t size) override {
        if (failed)
            return ESP_FAIL;
        esp_err_t rc = Partition::erase_range(offset, size);
        failed |= rc != ESP_OK;
        return rc;
    }
};
class guarded_wl final : public WL_Flash {
    guarded_partition physical;
    bool poisoned = false;
    bool checksum(const wl_state_t &s) {
        return s.crc32 == crc32::crc32_le(UINT32_MAX, (const uint8_t *)&s, WL_STATE_CRC_LEN_V2);
    }
    bool compatible(const wl_state_t &s) {
        return s.version == 2 && s.wl_block_size == erase_bytes &&
               s.wl_part_max_sec_pos == 1 + flash_size / erase_bytes &&
               s.wl_dummy_sec_pos < s.wl_part_max_sec_pos &&
               s.wl_dummy_sec_move_count < s.wl_part_max_sec_pos - 1 &&
               s.wl_max_sec_erase_cycle_count == 16 && s.wl_sec_erase_cycle_count < 16;
    }

  public:
    explicit guarded_wl(const esp_partition_t *p) : physical(p) {}
    int open(const esp_partition_t *p) {
        wl_config_t c = {};
        c.wl_partition_size = p->size;
        c.wl_page_size = c.flash_sector_size = erase_bytes;
        c.wl_update_rate = 16;
        c.wl_pos_update_record_size = 16;
        c.version = 2;
        c.wl_temp_buff_size = 32;
        int rc = config(&c, &physical);
        if (rc)
            return idf_error(rc);
        wl_state_t a, b;
        rc = physical.read(addr_state1, &a, sizeof(a));
        if (!rc)
            rc = physical.read(addr_state2, &b, sizeof(b));
        if (rc)
            return idf_error(rc);
        bool first = checksum(a), second = checksum(b);
        /* Do not let the original initializer interpret an incompatible
         * version/geometry as permission to recreate metadata. */
        if ((first && !compatible(a)) || (second && !compatible(b)))
            return -NOS_DRV_ENOTSUP;
        if (!first && !second) {
            uint8_t bytes[512];
            for (size_t offset = 0; offset < p->size; offset += sizeof(bytes)) {
                rc = physical.read(offset, bytes, sizeof(bytes));
                if (rc)
                    return idf_error(rc);
                for (uint8_t byte : bytes)
                    if (byte != 0xff)
                        return -NOS_DRV_EIO;
            }
        }
        /* Only fully erased storage may initialize metadata; an existing
         * valid copy may be repaired by the unmodified original algorithm. */
        rc = init();
        /* IDF's single-state repair branch leaves the RAM dummy position at
         * max-1. Reuse its original position replay after every init, so the
         * first reads after a repaired mount use the persisted mapping. */
        if (!rc)
            rc = recoverPos();
        return idf_error(rc);
    }
    int call(uint32_t command, void *request, size_t size) {
        if ((command == NOS_IO_READ || command == NOS_IO_WRITE) && request &&
            size == sizeof(nos_io_block))
            ((nos_io_block *)request)->actual = 0;
        if (command == NOS_IO_STATUS && request && size == sizeof(nos_io_block_info)) {
            *(nos_io_block_info *)request = {get_flash_size() / erase_bytes, logical_bytes};
            return 0;
        }
        if (poisoned || physical.failed)
            return -NOS_DRV_EIO;
        if (command == NOS_IO_COMMIT && !size) {
            int rc = flush();
            poisoned |= rc != ESP_OK;
            return idf_error(rc);
        }
        if ((command != NOS_IO_READ && command != NOS_IO_WRITE) || !request ||
            size != sizeof(nos_io_block))
            return -NOS_DRV_ENOTSUP;
        auto *b = (nos_io_block *)request;
        b->actual = 0;
        size_t sectors = get_flash_size() / erase_bytes;
        if (!b->data || !b->count || b->count > NOS_DRIVER_MAX_TRANSFER / logical_bytes ||
            b->sector >= sectors || b->count > sectors - b->sector)
            return -NOS_DRV_EINVAL;
        for (size_t i = 0; i < b->count; i++) {
            size_t offset = (b->sector + i) * erase_bytes;
            auto *data = (uint8_t *)b->data + i * logical_bytes;
            int rc;
            if (command == NOS_IO_READ) {
                rc = read(offset, data, logical_bytes);
                if (!rc)
                    for (size_t j = 0; j < logical_bytes; j++)
                        data[j] ^= 0xff;
            } else {
                uint8_t encoded[logical_bytes];
                for (size_t j = 0; j < logical_bytes; j++)
                    encoded[j] = data[j] ^ 0xff;
                rc = erase_range(offset, erase_bytes);
                if (!rc)
                    rc = write(offset, encoded, sizeof(encoded));
            }
            if (rc) {
                poisoned = true;
                return idf_error(rc);
            }
            b->actual++;
        }
        return 0;
    }
};
extern "C" int nos_wl_block_start(nos_flash_block *block) {
    if (block->partition || block->wear)
        return -NOS_DRV_EBUSY;
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "storage");
    if (!p)
        return -NOS_DRV_ENODEV;
    if (p->readonly || p->encrypted)
        return -NOS_DRV_ENOTSUP;
    if (p->type != ESP_PARTITION_TYPE_DATA || p->subtype != 0x41 || p->erase_size != erase_bytes ||
        p->address % erase_bytes || p->size % erase_bytes || p->size < 70 * erase_bytes)
        return -NOS_DRV_EINVAL;
    void *memory = aligned_alloc(alignof(guarded_wl), sizeof(guarded_wl));
    if (!memory)
        return -NOS_DRV_ENOSPC;
    auto *wl = new (memory) guarded_wl(p);
    int rc = wl->open(p);
    if (rc) {
        wl->~guarded_wl();
        free(wl);
        return rc;
    }
    block->partition = p;
    block->wear = wl;
    return 0;
}
extern "C" int nos_wl_block_stop(nos_flash_block *block) {
    if (block->wear) {
        /* Operations are synchronous. Discard uncertain RAM state after an
         * I/O fault; never flush it while releasing the NOS controller lease. */
        auto *wl = (guarded_wl *)block->wear;
        wl->~guarded_wl();
        free(wl);
    }
    block->wear = nullptr;
    block->partition = nullptr;
    return 0;
}
extern "C" int nos_wl_block_call(nos_flash_block *block, uint32_t command, void *request,
                                 size_t size) {
    return block->wear ? ((guarded_wl *)block->wear)->call(command, request, size)
                       : -NOS_DRV_ENODEV;
}
