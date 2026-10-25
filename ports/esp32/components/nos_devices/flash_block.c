/* Copyright (C) 2026 NOS contributors. Use original IDF NOR operations. */
#include "flash_block.h"
#include "esp_err.h"
#define LOGICAL_BYTES 512u
#define ERASE_BYTES 4096u
#define MIN_SECTORS 66u /* Two native VFS banks of 1 + 2 * 16 sectors. */
extern int idf_error(int error);

int nos_flash_block_start(struct nos_flash_block *block) {
    if (block->partition)
        return -NOS_DRV_EBUSY;
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "storage");
    if (!p)
        return -NOS_DRV_ENODEV;
    if (p->encrypted || p->readonly)
        return -NOS_DRV_ENOTSUP;
    if (p->type != ESP_PARTITION_TYPE_DATA || p->subtype != 0x40 || p->address % ERASE_BYTES ||
        p->size % ERASE_BYTES || p->size / ERASE_BYTES < MIN_SECTORS)
        return -NOS_DRV_EINVAL;
    /* Discovering/mounting never erases or writes an unknown partition. */
    block->partition = p;
    return 0;
}
int nos_flash_block_stop(struct nos_flash_block *block) {
    block->partition = NULL;
    return 0;
}
int nos_flash_block_call(struct nos_flash_block *block, uint32_t command, void *request,
                         size_t size) {
    const esp_partition_t *p = block->partition;
    if (!p)
        return -NOS_DRV_ENODEV;
    if (command == NOS_IO_STATUS && request && size == sizeof(struct nos_io_block_info)) {
        *(struct nos_io_block_info *)request =
            (struct nos_io_block_info){p->size / ERASE_BYTES, LOGICAL_BYTES};
        return 0;
    }
    /* IDF partition read/erase/write operations are synchronous. */
    if (command == NOS_IO_COMMIT && !size)
        return 0;
    if ((command != NOS_IO_READ && command != NOS_IO_WRITE) ||
        size != sizeof(struct nos_io_block) || !request)
        return -NOS_DRV_ENOTSUP;
    struct nos_io_block *b = request;
    b->actual = 0;
    uint64_t sectors = p->size / ERASE_BYTES;
    if (!b->data || !b->count || b->count > NOS_DRIVER_MAX_TRANSFER / LOGICAL_BYTES ||
        b->sector >= sectors || b->count > sectors - b->sector)
        return -NOS_DRV_EINVAL;
    for (size_t i = 0; i < b->count; i++) {
        size_t offset = (size_t)(b->sector + i) * ERASE_BYTES;
        unsigned char *data = (unsigned char *)b->data + i * LOGICAL_BYTES;
        int rc;
        if (command == NOS_IO_READ) {
            rc = esp_partition_read(p, offset, data, LOGICAL_BYTES);
            if (rc)
                return idf_error(rc);
            /* An erased NOR sector is the VFS's blank, zero-filled sector.
             * This is reversible for every byte, not an all-FF special case. */
            for (unsigned j = 0; j < LOGICAL_BYTES; j++)
                data[j] ^= 0xff;
        } else {
            unsigned char encoded[LOGICAL_BYTES];
            for (unsigned j = 0; j < LOGICAL_BYTES; j++)
                encoded[j] = data[j] ^ 0xff;
            /* A logical sector owns its entire erase block. In particular,
             * bank headers/payload never share an erase block with the other
             * bank, even though a native bank is 33 logical sectors long. */
            rc = esp_partition_erase_range(p, offset, ERASE_BYTES);
            if (!rc)
                rc = esp_partition_write(p, offset, encoded, sizeof(encoded));
            if (rc)
                return idf_error(rc);
        }
        b->actual++;
    }
    return 0;
}
