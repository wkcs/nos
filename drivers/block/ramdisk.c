/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <drivers/block.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/printk.h>

#define RAMDISK_SECTOR_SIZE 512U
#define RAMDISK_SECTOR_COUNT 512U
#define RAMDISK_SIZE (RAMDISK_SECTOR_SIZE * RAMDISK_SECTOR_COUNT)

static uint8_t ramdisk_storage[RAMDISK_SIZE];

static int ramdisk_validate_request(uint32_t lba, uint32_t count)
{
    if (lba >= RAMDISK_SECTOR_COUNT ||
        count == 0 || count > RAMDISK_SECTOR_COUNT - lba) {
        return -EINVAL;
    }

    return 0;
}

static int ramdisk_read(const struct device *dev, uint32_t lba, void *buf,
                        uint32_t count)
{
    uint8_t *dst = buf;
    uint32_t offset;
    uint32_t bytes;
    uint32_t i;
    int ret;

    (void)dev;

    if (buf == NULL) {
        return -EINVAL;
    }

    ret = ramdisk_validate_request(lba, count);
    if (ret) {
        return ret;
    }

    offset = lba * RAMDISK_SECTOR_SIZE;
    bytes = count * RAMDISK_SECTOR_SIZE;
    for (i = 0; i < bytes; i++) {
        dst[i] = ramdisk_storage[offset + i];
    }

    return 0;
}

static int ramdisk_write(const struct device *dev, uint32_t lba,
                         const void *buf, uint32_t count)
{
    const uint8_t *src = buf;
    uint32_t offset;
    uint32_t bytes;
    uint32_t i;
    int ret;

    (void)dev;

    if (buf == NULL) {
        return -EINVAL;
    }

    ret = ramdisk_validate_request(lba, count);
    if (ret) {
        return ret;
    }

    offset = lba * RAMDISK_SECTOR_SIZE;
    bytes = count * RAMDISK_SECTOR_SIZE;
    for (i = 0; i < bytes; i++) {
        ramdisk_storage[offset + i] = src[i];
    }

    return 0;
}

static int ramdisk_get_info(const struct device *dev, uint32_t *sector_size,
                            uint32_t *sector_count)
{
    (void)dev;

    if (sector_size == NULL || sector_count == NULL) {
        return -EINVAL;
    }

    *sector_size = RAMDISK_SECTOR_SIZE;
    *sector_count = RAMDISK_SECTOR_COUNT;

    return 0;
}

static int ramdisk_init(struct device *dev)
{
    (void)dev;

    return 0;
}

static const struct block_driver_api ramdisk_api = {
    .read = ramdisk_read,
    .write = ramdisk_write,
    .get_info = ramdisk_get_info,
};

DEVICE_DEFINE(ramdisk0, "ramdisk0", ramdisk_init, NULL, NULL, &ramdisk_api,
              POST_KERNEL);

static int ramdisk_smoke(void)
{
    struct device *dev;
    uint8_t write_buf[RAMDISK_SECTOR_SIZE];
    uint8_t read_buf[RAMDISK_SECTOR_SIZE];
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t i;

    dev = device_get_binding("ramdisk0");
    if (!device_is_ready(dev)) {
        pr_err("ramdisk smoke: device unavailable\r\n");
        return -ENODEV;
    }

    for (i = 0; i < RAMDISK_SECTOR_SIZE; i++) {
        write_buf[i] = (uint8_t)i;
        read_buf[i] = 0;
    }

    if (block_get_info(dev, &sector_size, &sector_count) ||
        sector_size != RAMDISK_SECTOR_SIZE || sector_count == 0 ||
        block_write(dev, 1, write_buf, 1) ||
        block_read(dev, 1, read_buf, 1)) {
        pr_err("ramdisk smoke: I/O failed\r\n");
        return -EIO;
    }

    for (i = 0; i < RAMDISK_SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            pr_err("ramdisk smoke: data mismatch\r\n");
            return -EIO;
        }
    }

    pr_info("ramdisk smoke OK\r\n");
    return 0;
}

/* APPLICATION: after DEVICE_DEFINE registration; use LBA 1 to avoid FatFs boot sector. */
SYS_INIT(ramdisk_smoke, INIT_LEVEL_APPLICATION);
