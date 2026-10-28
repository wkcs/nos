/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>
#include <stdio.h>
#include <string.h>

static int active, phase, writing, bad_token, bad_crc, failed, idle, sent_data;
static int token_stuck, busy_stuck, rejected;
static uint32_t argument;
static uint8_t cmd;
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        int b;
        crc ^= (uint16_t)*p++ << 8;
        for (b = 0; b < 8; b++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
static int begin(const struct device *dev, const struct spi_config *cfg)
{
    (void)dev; (void)cfg;
    if (active) return -EBUSY;
    active = 1; phase = 0; return 0;
}
static int transfer(const struct device *dev, const struct spi_buf *b, size_t count)
{
    size_t i;
    (void)dev;
    if (!active || count != 1) return -EIO;
    if (failed) return -EIO;
    if (phase == 0 && b->tx && b->len == 6) {
        const uint8_t *p = b->tx;
        cmd = p[0] & 63;
        argument = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
        writing = cmd == 24; phase = 1; return 0;
    }
    if (b->rx) {
        uint8_t *p = b->rx;
        if (phase == 1 && b->len == 1) { p[0] = 0; phase = 2; return 0; }
        if (!writing && phase == 2 && b->len == 1) {
            p[0] = token_stuck ? 0xff : bad_token ? 0x0b : 0xfe;
            if (!token_stuck) phase = 3;
            return 0;
        }
        if (!writing && phase == 3 && b->len == 512) {
            for (i = 0; i < b->len; i++) p[i] = (uint8_t)i;
            phase = 4; return 0;
        }
        if (!writing && phase == 4 && b->len == 2) {
            uint8_t data[512]; uint16_t crc;
            for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
            crc = crc16(data, sizeof(data));
            p[0] = (uint8_t)(crc >> 8); p[1] = (uint8_t)crc ^ (bad_crc ? 1 : 0);
            phase = 5; return 0;
        }
        if (writing && phase == 5 && b->len == 1) { p[0] = rejected ? 0x0b : 5; phase = 6; return 0; }
        if (writing && phase == 6 && b->len == 1) {
            p[0] = busy_stuck ? 0 : 0xff;
            if (!busy_stuck) phase = 7;
            return 0;
        }
    } else if (b->tx && writing) {
        const uint8_t *p = b->tx;
        if (phase == 2 && b->len == 1 && p[0] == 0xfe) { phase = 3; return 0; }
        if (phase == 3 && b->len == 512) { sent_data = memcmp(p, (uint8_t[512]){0}, 512) == 0; phase = 4; return 0; }
        if (phase == 4 && b->len == 2 && p[0] == 0 && p[1] == 0) { phase = 5; return 0; }
    }
    return -EINVAL;
}
static int end(const struct device *dev) { (void)dev; active = 0; return 0; }
static int clock_idle(const struct device *dev, const struct spi_config *cfg, size_t bytes)
{
    (void)dev; (void)cfg; if (active || bytes != 1) return -EIO; idle++; return 0;
}
int main(void)
{
    const struct spi_driver_api api = { .session_begin = begin, .session_transfer = transfer,
        .session_end = end, .clock_idle = clock_idle };
    const struct device bus = { .api = &api, .ready = true };
    struct sd_spi_card card = { .spi = { .bus = &bus, .config = { .frequency = 1000000,
        .word_size = 8, .mode = 0 } }, .high_capacity = 1, .ready = 1 };
    uint8_t buffer[512] = {0};
    if (sd_spi_read_block(&card, 7, buffer) != 0 || cmd != 17 || argument != 7 ||
        buffer[257] != 1 || active) return 1;
    memset(buffer, 0, sizeof(buffer));
    card.high_capacity = 0;
    if (sd_spi_write_block(&card, 7, buffer) != 0 || cmd != 24 || argument != 3584 ||
        !sent_data || active) return 1;
    bad_token = 1;
    if (sd_spi_read_block(&card, 1, buffer) != -EIO || active) return 1;
    bad_token = 0; bad_crc = 1;
    if (sd_spi_read_block(&card, 1, buffer) != -EIO || active) return 1;
    bad_crc = 0; failed = 1;
    if (sd_spi_read_block(&card, 1, buffer) != -EIO || active) return 1;
    failed = 0; token_stuck = 1;
    if (sd_spi_read_block(&card, 1, buffer) != -ETIMEDOUT || active) return 1;
    token_stuck = 0; busy_stuck = 1;
    memset(buffer, 0, sizeof(buffer));
    if (sd_spi_write_block(&card, 1, buffer) != -ETIMEDOUT || active) return 1;
    busy_stuck = 0; rejected = 1;
    if (sd_spi_write_block(&card, 1, buffer) != -EIO || active) return 1;
    if (sd_spi_read_block(&card, 0x800000, buffer) != -EINVAL || active) return 1;
    if (idle != 8) return 1;
    puts("SD SPI block I/O checks passed");
    return 0;
}
