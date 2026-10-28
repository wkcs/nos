/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_W25Q_SPI_H__
#define __NOS_DRIVERS_W25Q_SPI_H__

#include <drivers/spi.h>

struct w25q_spi {
    const struct device *bus;
    struct spi_config config;
};

/* 24-bit read command: address and length must fit within 16 MiB. */
int w25q_spi_read(const struct w25q_spi *flash, uint32_t address,
                  void *buffer, size_t length);

#endif /* __NOS_DRIVERS_W25Q_SPI_H__ */
