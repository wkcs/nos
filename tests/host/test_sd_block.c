/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_block.h>
#include <stdio.h>

static struct device *device;
static int calls, error_at, lock_error;
static uint32_t expected_lba;
int sd_spi_card_init(struct sd_spi_card *card, const struct sd_spi *spi)
{
    card->spi = *spi; card->ready = 1; card->sector_count = 8; return 0;
}
int sd_spi_read_block(struct sd_spi_card *card, uint32_t lba, void *buf)
{
    (void)card;
    if (!device->mutex.locked || lba != expected_lba++ || buf == NULL) lock_error = 1;
    calls++;
    return calls == error_at ? -EIO : 0;
}
int sd_spi_write_block(struct sd_spi_card *card, uint32_t lba, const void *buf)
{
    return sd_spi_read_block(card, lba, (void *)buf);
}
void mutex_lock(struct mutex *m) { if (m->locked) lock_error = 1; m->locked = 1; }
void mutex_unlock(struct mutex *m) { if (!m->locked) lock_error = 1; m->locked = 0; }
int main(void)
{
    struct sd_spi_card card = {0};
    const struct sd_spi spi = {0};
    struct device dev = { .config = &spi, .data = &card, .api = &sd_block_driver_api };
    uint8_t data[1536];
    uint32_t size, count;
    device = &dev;
    if (sd_block_init(&dev) != 0) return 1;
    dev.ready = 1;
    if (block_get_info(&dev, &size, &count) || size != 512 || count != 8) return 1;
    expected_lba = 3;
    if (block_read(&dev, 3, data, 3) != 0 || calls != 3 || lock_error || dev.mutex.locked) return 1;
    calls = 0; expected_lba = 2; error_at = 2;
    if (block_write(&dev, 2, data, 3) != -EIO || calls != 2 || dev.mutex.locked) return 1;
    calls = 0;
    if (block_read(&dev, 7, data, 2) != -EINVAL || block_read(&dev, 0, data, 0) != -EINVAL || calls) return 1;
    puts("SD block adapter checks passed");
    return 0;
}
