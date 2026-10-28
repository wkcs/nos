/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>
#include <kernel/sleep.h>

#define SD_INIT_FREQUENCY 400000U
#define SD_INIT_RETRIES 1000U

/* The extra clocks allow the card to release MISO before another bus user. */
static int sd_command(const struct sd_spi *spi, uint8_t command, uint32_t argument,
                      uint8_t *response, uint8_t *extra, size_t extra_length)
{
    struct spi_session session = {0};
    struct spi_buf buffer;
    int ret, cleanup;

    ret = sd_spi_begin_command(spi, &session, command, argument, response);
    if (ret == 0 && extra_length != 0 && (*response & 0x04U) == 0) {
        buffer = (struct spi_buf) { .rx = extra, .len = extra_length };
        ret = spi_session_transfer(&session, &buffer, 1);
    }
    if (session.active) {
        cleanup = spi_session_end(&session);
        if (ret == 0) ret = cleanup;
    }
    cleanup = spi_clock_idle(spi->bus, &spi->config, 1);
    if (ret == 0) ret = cleanup;
    return ret;
}

int sd_spi_card_init(struct sd_spi_card *card, const struct sd_spi *spi)
{
    struct sd_spi init_spi;
    uint8_t response, extra[4] = {0};
    unsigned int retries;
    int version2, ret;

    if (card == NULL || spi == NULL) return -EINVAL;
    card->ready = 0;
    card->high_capacity = 0;
    if (spi->config.frequency == 0 || spi->config.word_size != 8 || spi->config.mode != 0) {
        return -EINVAL;
    }
    init_spi = *spi;
    if (init_spi.config.frequency > SD_INIT_FREQUENCY) {
        init_spi.config.frequency = SD_INIT_FREQUENCY;
    }
    ret = spi_clock_idle(init_spi.bus, &init_spi.config, 10);
    if (ret != 0) return ret;
    ret = sd_command(&init_spi, 0, 0, &response, NULL, 0);
    if (ret != 0) return ret;
    if (response != 1) return -EIO;
    ret = sd_command(&init_spi, 8, 0x1aaU, &response, extra, sizeof(extra));
    if (ret != 0) return ret;
    if (response == 1) {
        if (extra[2] != 1 || extra[3] != 0xaa) return -EIO;
        version2 = 1;
    } else if (response == 5) {
        version2 = 0;
    } else {
        return -EIO;
    }
    for (retries = 0; retries < SD_INIT_RETRIES; retries++) {
        ret = sd_command(&init_spi, 55, 0, &response, NULL, 0);
        if (ret != 0) return ret;
        if (response > 1) return -EIO;
        ret = sd_command(&init_spi, 41, version2 ? 0x40000000U : 0,
                         &response, NULL, 0);
        if (ret != 0) return ret;
        if (response == 0) break;
        if (response != 1) return -EIO;
        msleep(1);
    }
    if (retries == SD_INIT_RETRIES) return -ETIMEDOUT;
    ret = sd_command(&init_spi, 58, 0, &response, extra, sizeof(extra));
    if (ret != 0) return ret;
    if (response != 0 || (extra[0] & 0x80U) == 0 ||
        (extra[1] == 0 && (extra[2] & 0x80U) == 0)) return -EIO;
    if (version2 && (extra[0] & 0x40U) != 0) {
        card->high_capacity = 1;
    } else {
        ret = sd_command(&init_spi, 16, 512, &response, NULL, 0);
        if (ret != 0) return ret;
        if (response != 0) return -EIO;
    }
    card->spi = *spi;
    card->ready = 1;
    return 0;
}
