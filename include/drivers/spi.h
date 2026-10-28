/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */
#ifndef __NOS_DRIVERS_SPI_H__
#define __NOS_DRIVERS_SPI_H__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <stddef.h>
#include <stdint.h>

struct spi_config {
    uint32_t frequency;
    uint16_t chip_select;
    uint8_t word_size;
    uint8_t mode;
};

/* NULL tx sends all ones; NULL rx discards received data. */
struct spi_buf {
    const void *tx;
    void *rx;
    size_t len;
};

struct spi_driver_api {
    /* Serialize the whole transaction and hold chip select across all buffers. */
    int (*transceive)(const struct device *dev, const struct spi_config *config,
                      const struct spi_buf *buffers, size_t count);
};

static inline int spi_transceive(const struct device *dev,
                                 const struct spi_config *config,
                                 const struct spi_buf *buffers, size_t count)
{
    const struct spi_driver_api *api;
    size_t i;

    if (config == NULL || buffers == NULL || count == 0 ||
        config->frequency == 0 || config->word_size == 0 || config->mode > 3) {
        return -EINVAL;
    }
    for (i = 0; i < count; i++) {
        if (buffers[i].len == 0 || (buffers[i].tx == NULL && buffers[i].rx == NULL)) {
            return -EINVAL;
        }
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    api = dev->api;
    if (api == NULL || api->transceive == NULL) {
        return -ENOSYS;
    }
    return api->transceive(dev, config, buffers, count);
}

#endif /* __NOS_DRIVERS_SPI_H__ */
