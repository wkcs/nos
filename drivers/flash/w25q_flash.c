/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_flash.h>
#include <kernel/mm.h>
#include <kernel/sleep.h>
#include <string.h>

#define W25Q_SECTOR_SIZE 4096U
#define W25Q_PAGE_SIZE 256U
#define W25Q_MAX_CAPACITY 0x1000000U
#define W25Q_POLL_LIMIT 2000U

static int w25q_wait_ready(const struct w25q_spi *spi)
{
    uint32_t remaining = W25Q_POLL_LIMIT;
    uint8_t status;
    int ret;

    while (remaining-- != 0) {
        ret = w25q_spi_read_status(spi, &status);
        if (ret != 0) {
            return ret;
        }
        if ((status & 1U) == 0) {
            return 0;
        }
        msleep(1);
    }
    return -ETIMEDOUT;
}

static int w25q_program(const struct w25q_spi *spi, uint32_t address,
                        const uint8_t *data, size_t length)
{
    size_t chunk;
    int ret;

    while (length != 0) {
        chunk = W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE);
        if (chunk > length) chunk = length;
        ret = w25q_spi_write_enable(spi);
        if (ret == 0) ret = w25q_spi_program_page(spi, address, data, chunk);
        if (ret == 0) ret = w25q_wait_ready(spi);
        if (ret != 0) return ret;
        address += (uint32_t)chunk;
        data += chunk;
        length -= chunk;
    }
    return 0;
}

int w25q_flash_init(struct w25q_flash *flash, const struct w25q_spi *spi,
                    uint32_t capacity)
{
    if (flash == NULL || spi == NULL || capacity == 0 ||
        capacity > W25Q_MAX_CAPACITY || capacity % W25Q_SECTOR_SIZE != 0) {
        return -EINVAL;
    }
    flash->spi = spi;
    flash->capacity = capacity;
    return mutex_init(&flash->lock);
}

int w25q_flash_read(struct w25q_flash *flash, uint32_t address,
                    void *buffer, size_t length)
{
    int ret;

    if (flash == NULL || address > flash->capacity ||
        length > flash->capacity - address || (length != 0 && buffer == NULL)) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    if (ret == 0) {
        ret = w25q_spi_read(flash->spi, address, buffer, length);
    }
    mutex_unlock(&flash->lock);
    return ret;
}

int w25q_flash_write(struct w25q_flash *flash, uint32_t address,
                     const void *buffer, size_t length)
{
    const uint8_t *source = buffer;
    uint8_t *sector;
    size_t remaining = length;
    size_t chunk;
    size_t offset;
    size_t page;
    size_t i;
    uint32_t sector_address;
    int erase_required;
    int ret = 0;

    if (flash == NULL || address > flash->capacity ||
        length > flash->capacity - address || (length != 0 && buffer == NULL)) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    sector = kmalloc(W25Q_SECTOR_SIZE, GFP_KERNEL);
    if (sector == NULL) {
        return -ENOMEM;
    }
    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    while (remaining != 0 && ret == 0) {
        sector_address = address & ~(W25Q_SECTOR_SIZE - 1U);
        offset = address - sector_address;
        chunk = W25Q_SECTOR_SIZE - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }
        ret = w25q_spi_read(flash->spi, sector_address, sector, W25Q_SECTOR_SIZE);
        if (ret != 0) {
            break;
        }
        if (memcmp(sector + offset, source, chunk) != 0) {
            erase_required = 0;
            for (i = 0; i < chunk; i++) {
                if ((sector[offset + i] & source[i]) != source[i]) {
                    erase_required = 1;
                    break;
                }
            }
            if (!erase_required) {
                ret = w25q_program(flash->spi, address, source, chunk);
            } else {
                memcpy(sector + offset, source, chunk);
                ret = w25q_spi_write_enable(flash->spi);
                if (ret == 0) ret = w25q_spi_erase_sector(flash->spi, sector_address);
                if (ret == 0) ret = w25q_wait_ready(flash->spi);
                for (page = 0; page < W25Q_SECTOR_SIZE && ret == 0; page += W25Q_PAGE_SIZE) {
                    for (i = 0; i < W25Q_PAGE_SIZE && sector[page + i] == 0xffU; i++) {
                    }
                    if (i != W25Q_PAGE_SIZE) {
                        ret = w25q_program(flash->spi, sector_address + (uint32_t)page,
                                           sector + page, W25Q_PAGE_SIZE);
                    }
                }
            }
            if (ret != 0) break;
        }
        source += chunk;
        address += (uint32_t)chunk;
        remaining -= chunk;
    }
    mutex_unlock(&flash->lock);
    kfree(sector);
    return ret;
}
