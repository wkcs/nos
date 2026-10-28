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
    int (*session_begin)(const struct device *dev, const struct spi_config *config);
    int (*session_transfer)(const struct device *dev, const struct spi_buf *buffers,
                            size_t count);
    int (*session_end)(const struct device *dev);
};

/* Initialize to zero and end in the task that began it. The bus stays locked;
 * do not start another transaction on that controller from the same task. */
struct spi_session {
    const struct device *dev;
    const struct spi_driver_api *api;
    uint8_t active;
};

static inline int spi_buffers_valid(const struct spi_buf *buffers, size_t count)
{
    size_t i;

    if (buffers == NULL || count == 0) return 0;
    for (i = 0; i < count; i++) {
        if (buffers[i].len == 0 || (buffers[i].tx == NULL && buffers[i].rx == NULL)) {
            return 0;
        }
    }
    return 1;
}

static inline int spi_transceive(const struct device *dev,
                                 const struct spi_config *config,
                                 const struct spi_buf *buffers, size_t count)
{
    const struct spi_driver_api *api;
    if (config == NULL || !spi_buffers_valid(buffers, count) ||
        config->frequency == 0 || config->word_size == 0 || config->mode > 3) {
        return -EINVAL;
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

static inline int spi_session_begin(struct spi_session *session, const struct device *dev,
                                    const struct spi_config *config)
{
    const struct spi_driver_api *api;
    int ret;

    if (session == NULL || config == NULL || config->frequency == 0 ||
        config->word_size == 0 || config->mode > 3) return -EINVAL;
    if (session->active) return -EBUSY;
    if (!device_is_ready(dev)) return -ENODEV;
    api = dev->api;
    if (api == NULL || api->session_begin == NULL || api->session_transfer == NULL ||
        api->session_end == NULL) return -ENOSYS;
    ret = api->session_begin(dev, config);
    if (ret == 0) {
        session->dev = dev;
        session->api = api;
        session->active = 1;
    }
    return ret;
}

static inline int spi_session_end(struct spi_session *session)
{
    int ret;

    if (session == NULL || !session->active) return -EINVAL;
    ret = session->api->session_end(session->dev);
    session->active = 0;
    session->dev = NULL;
    session->api = NULL;
    return ret;
}

/* A controller error ends the session. Invalid arguments leave it active so
 * the caller can retry or explicitly end it. */
static inline int spi_session_transfer(struct spi_session *session,
                                       const struct spi_buf *buffers, size_t count)
{
    int ret;

    if (session == NULL || !session->active || !spi_buffers_valid(buffers, count)) {
        return -EINVAL;
    }
    ret = session->api->session_transfer(session->dev, buffers, count);
    if (ret != 0) (void)spi_session_end(session);
    return ret;
}

#endif /* __NOS_DRIVERS_SPI_H__ */
