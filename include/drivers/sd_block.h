/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_SD_BLOCK_H__
#define __NOS_DRIVERS_SD_BLOCK_H__

#include <drivers/block.h>
#include <drivers/sd_spi.h>

/* dev->config points to struct sd_spi, dev->data to struct sd_spi_card.
 * Call after device_init(), from a task where msleep() is available. */
int sd_block_init(struct device *dev);
extern const struct block_driver_api sd_block_driver_api;

#endif /* __NOS_DRIVERS_SD_BLOCK_H__ */
