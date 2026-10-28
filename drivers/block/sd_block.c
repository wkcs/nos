/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_block.h>

#define SD_SECTOR_SIZE 512U

static int sd_block_request(const struct device *dev, uint32_t lba,
                            void *buffer, uint32_t count, int write)
{
    struct sd_spi_card *card = dev->data;
    struct device *device = (struct device *)dev;
    uint8_t *data = buffer;
    uint32_t i;
    int ret = 0;

    if (card == NULL || buffer == NULL || count == 0 || lba >= card->sector_count ||
        count > card->sector_count - lba || (uint64_t)count * SD_SECTOR_SIZE > SIZE_MAX) {
        return -EINVAL;
    }
    mutex_lock(&device->mutex);
    for (i = 0; i < count; i++) {
        if (write) ret = sd_spi_write_block(card, lba + i, data);
        else ret = sd_spi_read_block(card, lba + i, data);
        if (ret != 0) break;
        data += SD_SECTOR_SIZE;
    }
    mutex_unlock(&device->mutex);
    return ret;
}

static int sd_block_read(const struct device *dev, uint32_t lba, void *buffer, uint32_t count)
{
    return sd_block_request(dev, lba, buffer, count, 0);
}

static int sd_block_write(const struct device *dev, uint32_t lba, const void *buffer,
                          uint32_t count)
{
    return sd_block_request(dev, lba, (void *)buffer, count, 1);
}

static int sd_block_get_info(const struct device *dev, uint32_t *size, uint32_t *count)
{
    const struct sd_spi_card *card = dev->data;

    if (card == NULL || size == NULL || count == NULL) return -EINVAL;
    *size = SD_SECTOR_SIZE;
    *count = card->sector_count;
    return 0;
}

int sd_block_init(struct device *dev)
{
    if (dev == NULL || dev->config == NULL || dev->data == NULL) return -EINVAL;
    return sd_spi_card_init(dev->data, dev->config);
}

const struct block_driver_api sd_block_driver_api = {
    .read = sd_block_read,
    .write = sd_block_write,
    .get_info = sd_block_get_info,
};
