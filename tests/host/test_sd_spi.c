/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>
#include <stdio.h>
#include <string.h>

static int active, calls, ends, timeout, fail;
static uint8_t command[6];

static int begin(const struct device *dev, const struct spi_config *config)
{
    (void)dev; (void)config;
    if (active) return -EBUSY;
    active = 1;
    return 0;
}
static int transfer(const struct device *dev, const struct spi_buf *buffers, size_t count)
{
    (void)dev;
    if (!active || count != 1) return -EIO;
    if (fail) return -EIO;
    if (buffers[0].tx != NULL && buffers[0].len == 6) {
        memcpy(command, buffers[0].tx, 6);
        calls = 0;
    } else if (buffers[0].rx != NULL && buffers[0].len == 1) {
        *(uint8_t *)buffers[0].rx = timeout || calls++ == 0 ? 0xff : 0x01;
    } else return -EINVAL;
    return 0;
}
static int end(const struct device *dev)
{
    (void)dev;
    active = 0;
    ends++;
    return 0;
}
int main(void)
{
    const struct spi_driver_api api = { .session_begin = begin,
        .session_transfer = transfer, .session_end = end };
    const struct device bus = { .api = &api, .ready = true };
    const struct sd_spi sd = { .bus = &bus, .config = {
        .frequency = 400000, .word_size = 8, .mode = 0 } };
    struct spi_session session = {0};
    uint8_t response;
    const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
    const uint8_t cmd8[] = {0x48, 0, 0, 1, 0xaa, 0x87};

    if (sd_spi_begin_command(&sd, &session, 0, 0, &response) != 0 ||
        response != 1 || !active || memcmp(command, cmd0, 6) ||
        spi_session_end(&session) != 0) return 1;
    if (sd_spi_begin_command(&sd, &session, 8, 0x1aa, &response) != 0 ||
        memcmp(command, cmd8, 6) || spi_session_end(&session) != 0) return 1;
    timeout = 1;
    if (sd_spi_begin_command(&sd, &session, 0, 0, &response) != -ETIMEDOUT ||
        active || session.active || ends != 3) return 1;
    timeout = 0;
    fail = 1;
    if (sd_spi_begin_command(&sd, &session, 0, 0, &response) != -EIO ||
        active || session.active || ends != 4) return 1;
    if (sd_spi_begin_command(&sd, &session, 64, 0, &response) != -EINVAL ||
        sd_spi_begin_command(&sd, &session, 0, 0, NULL) != -EINVAL) return 1;
    puts("SD SPI command checks passed");
    return 0;
}
