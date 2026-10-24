/* Copyright (C) 2026 NOS contributors. */
/* W25Q protocol extracted from boards/nk60_v2/flash.c.
 * Copyright (C) 2023 胡启航<Nick Hu>, huqihan@live.com */
#include "driver/spi_nor.h"

static int valid(struct nos_spi_nor *n)
{
    return n && n->spi.bus && n->time_us && n->delay_us;
}
static void progress(struct nos_spi_nor *n)
{
    if (n->progress)
        n->progress(n->context);
}
static int command(struct nos_spi_nor *n, uint8_t op, uint32_t address, const void *tx, void *rx,
                   size_t count, int addressed)
{
    uint8_t header[4] = {op, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address};
    struct nos_spi_segment segments[2] = {{header, NULL, addressed ? 4u : 1u, 0xff},
                                          {tx, rx, count, 0xff}};
    return nos_spi_transfer(&n->spi, segments, count ? 2 : 1, 20000);
}
static int wait_ready(struct nos_spi_nor *n, uint32_t timeout)
{
    uint32_t start = n->time_us(n->context);
    unsigned i;
    for (i = 0; i < 100000u; i++) {
        uint8_t status;
        int error = command(n, 0x05, 0, NULL, &status, 1, 0);
        if (error)
            return error;
        if (!(status & 1))
            return 0;
        if ((uint32_t)(n->time_us(n->context) - start) >= timeout)
            return -NOS_DRV_ETIMEDOUT;
        progress(n);
        n->delay_us(n->context, 10);
    }
    return -NOS_DRV_ETIMEDOUT;
}
static int write_enable(struct nos_spi_nor *n)
{
    uint8_t status;
    int error = wait_ready(n, 1000000);
    if (!error)
        error = command(n, 0x06, 0, NULL, NULL, 0, 0);
    if (!error)
        error = command(n, 0x05, 0, NULL, &status, 1, 0);
    if (!error && !(status & 2))
        error = -NOS_DRV_EIO;
    return error;
}
static int range(struct nos_spi_nor *n, uint32_t offset, const void *buffer, size_t count)
{
    if (!valid(n) || (!buffer && count))
        return -NOS_DRV_EINVAL;
    if (!n->ready)
        return -NOS_DRV_ENODEV;
    if (offset > n->capacity || count > n->capacity - offset)
        return -NOS_DRV_EINVAL;
    return 0;
}
int nos_spi_nor_probe(struct nos_spi_nor *n, uint32_t *jedec_id)
{
    uint8_t id[3];
    int error;
    if (!valid(n))
        return -NOS_DRV_EINVAL;
    n->ready = 0;
    n->capacity = 0;
    error = command(n, 0xab, 0, NULL, NULL, 0, 0);
    if (error)
        return error;
    n->delay_us(n->context, 3);
    error = command(n, 0x9f, 0, NULL, id, sizeof(id), 0);
    if (error)
        return error;
    if (jedec_id)
        *jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
    if (id[0] != 0xef || (id[1] != 0x40 && id[1] != 0x60) || id[2] < 0x14 || id[2] > 0x18)
        return -NOS_DRV_ENODEV;
    n->capacity = 1u << id[2];
    n->ready = 1;
    return 0;
}
int nos_spi_nor_read(struct nos_spi_nor *n, uint32_t offset, void *data, size_t count)
{
    uint8_t *bytes = data;
    int error = range(n, offset, data, count);
    if (error || !count)
        return error;
    error = wait_ready(n, 1000000);
    if (error)
        return error;
    while (count) {
        size_t part = count > 64 ? 64 : count;
        error = command(n, 0x03, offset, NULL, bytes, part, 1);
        if (error)
            return error;
        progress(n);
        bytes += part;
        offset += (uint32_t)part;
        count -= part;
    }
    return 0;
}
int nos_spi_nor_program(struct nos_spi_nor *n, uint32_t offset, const void *data, size_t count)
{
    const uint8_t *bytes = data;
    int error = range(n, offset, data, count);
    if (error)
        return error;
    while (count) {
        size_t i, part = NOS_SPI_NOR_PAGE - (offset % NOS_SPI_NOR_PAGE);
        if (part > count)
            part = count;
        error = write_enable(n);
        if (!error)
            error = command(n, 0x02, offset, bytes, NULL, part, 1);
        if (!error)
            error = wait_ready(n, 1000000);
        if (error)
            return error;
        for (i = 0; i < part;) {
            uint8_t actual[64];
            size_t j, chunk = part - i;
            if (chunk > sizeof(actual))
                chunk = sizeof(actual);
            error = nos_spi_nor_read(n, offset + (uint32_t)i, actual, chunk);
            if (error)
                return error;
            for (j = 0; j < chunk; j++)
                if (actual[j] != bytes[i + j])
                    return -NOS_DRV_EIO;
            i += chunk;
        }
        offset += (uint32_t)part;
        bytes += part;
        count -= part;
    }
    return 0;
}
int nos_spi_nor_erase(struct nos_spi_nor *n, uint32_t offset)
{
    uint32_t i;
    int error = range(n, offset, NULL, 0);
    if (error)
        return error;
    if (n->capacity < NOS_SPI_NOR_SECTOR || offset % NOS_SPI_NOR_SECTOR ||
        offset > n->capacity - NOS_SPI_NOR_SECTOR)
        return -NOS_DRV_EINVAL;
    error = write_enable(n);
    if (!error)
        error = command(n, 0x20, offset, NULL, NULL, 0, 1);
    if (!error)
        error = wait_ready(n, 3000000);
    if (error)
        return error;
    for (i = 0; i < NOS_SPI_NOR_SECTOR; i += 64) {
        uint8_t actual[64];
        unsigned j;
        error = nos_spi_nor_read(n, offset + i, actual, sizeof(actual));
        if (error)
            return error;
        for (j = 0; j < sizeof(actual); j++)
            if (actual[j] != 0xff)
                return -NOS_DRV_EIO;
    }
    return 0;
}
