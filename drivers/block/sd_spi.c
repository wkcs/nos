/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>

#define SD_SPI_RESPONSE_BYTES 8U

static uint8_t sd_spi_crc7(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; i++) {
        uint8_t value = data[i];
        for (bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if ((value ^ crc) & 0x80U) crc ^= 0x09U;
            value <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1U);
}

int sd_spi_begin_command(const struct sd_spi *sd, struct spi_session *session,
                         uint8_t command, uint32_t argument, uint8_t *response)
{
    uint8_t frame[6];
    struct spi_buf buffer;
    unsigned int i;
    int ret;

    if (sd == NULL || session == NULL || response == NULL || command > 63U) return -EINVAL;
    frame[0] = 0x40U | command;
    frame[1] = (uint8_t)(argument >> 24);
    frame[2] = (uint8_t)(argument >> 16);
    frame[3] = (uint8_t)(argument >> 8);
    frame[4] = (uint8_t)argument;
    frame[5] = sd_spi_crc7(frame, 5);
    ret = spi_session_begin(session, sd->bus, &sd->config);
    if (ret != 0) return ret;
    buffer = (struct spi_buf) { .tx = frame, .len = sizeof(frame) };
    ret = spi_session_transfer(session, &buffer, 1);
    if (ret != 0) return ret;
    buffer = (struct spi_buf) { .rx = response, .len = 1 };
    for (i = 0; i < SD_SPI_RESPONSE_BYTES; i++) {
        ret = spi_session_transfer(session, &buffer, 1);
        if (ret != 0) return ret;
        if ((*response & 0x80U) == 0) return 0;
    }
    (void)spi_session_end(session);
    return -ETIMEDOUT;
}
