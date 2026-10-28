/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_SD_SPI_H__
#define __NOS_DRIVERS_SD_SPI_H__

#include <drivers/spi.h>

struct sd_spi {
    const struct device *bus;
    struct spi_config config;
};

struct sd_spi_card {
    struct sd_spi spi;
    uint8_t high_capacity;
    uint8_t ready;
};

/* Call from task context, with exclusive access to the card. */
int sd_spi_card_init(struct sd_spi_card *card, const struct sd_spi *spi);

/* Pass an inactive session. On success, the caller owns the active session and
 * must end it after any command-specific response or data. On failure, a
 * session opened by this function is closed. */
int sd_spi_begin_command(const struct sd_spi *sd, struct spi_session *session,
                         uint8_t command, uint32_t argument, uint8_t *response);

#endif /* __NOS_DRIVERS_SD_SPI_H__ */
