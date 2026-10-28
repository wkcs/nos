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

int w25q_spi_write_enable(const struct w25q_spi *flash);
int w25q_spi_read_status(const struct w25q_spi *flash, uint8_t *status);
int w25q_spi_read_id(const struct w25q_spi *flash, uint16_t *id);
int w25q_spi_erase_sector(const struct w25q_spi *flash, uint32_t address);
int w25q_spi_program_page(const struct w25q_spi *flash, uint32_t address,
                          const void *buffer, size_t length);

#endif /* __NOS_DRIVERS_W25Q_SPI_H__ */
