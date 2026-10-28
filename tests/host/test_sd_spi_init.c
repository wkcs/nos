/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>
#include <stdio.h>
#include <string.h>

static int active, high_capacity, v1, idle_bytes, tries, delays, invalid_echo, fail_end, stuck;
static uint8_t reply[20];
static size_t reply_len, reply_pos;
static uint8_t last_cmd;
static uint32_t last_arg;
static uint32_t highest_frequency;
static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    while (length--) {
        int bit;
        crc ^= (uint16_t)*data++ << 8;
        for (bit = 0; bit < 8; bit++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
void msleep(uint32_t ms) { delays += (int)ms; }
static int begin(const struct device *dev, const struct spi_config *cfg)
{
    (void)dev;
    if (active || cfg->frequency > 400000 || cfg->mode != 0) return -EIO;
    active = 1;
    if (cfg->frequency > highest_frequency) highest_frequency = cfg->frequency;
    return 0;
}
static int transfer(const struct device *dev, const struct spi_buf *b, size_t count)
{
    const uint8_t *p;
    (void)dev;
    if (!active || count != 1) return -EIO;
    if (b->tx != NULL && b->len == 6) {
        p = b->tx;
        last_cmd = p[0] & 0x3f;
        last_arg = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 8) | p[4];
        memset(reply, 0, sizeof(reply));
        reply_pos = 0; reply_len = 1;
        switch (last_cmd) {
        case 0: reply[0] = 1; break;
        case 8:
            if (v1) reply[0] = 5;
            else { reply[0] = 1; reply[3] = 1; reply[4] = invalid_echo ? 0 : 0xaa; reply_len = 5; }
            break;
        case 55: reply[0] = 1; break;
        case 41:
            if (last_arg != (v1 ? 0U : 0x40000000U)) return -EINVAL;
            reply[0] = ++tries >= 3 && !stuck ? 0 : 1; break;
        case 58: reply[0] = 0; reply[1] = 0x80 | (high_capacity ? 0x40 : 0);
            reply[2] = 0xff; reply_len = 5; break;
        case 16: if (last_arg != 512) return -EINVAL; reply[0] = 0; break;
        case 9: {
            uint16_t crc;
            reply[0] = 0; reply[1] = 0xfe;
            if (v1) reply[7] = 9;
            else reply[2] = 0x40;
            crc = crc16(reply + 2, 16);
            reply[18] = (uint8_t)(crc >> 8); reply[19] = (uint8_t)crc;
            reply_len = 20; break;
        }
        default: return -EINVAL;
        }
    } else if (b->rx != NULL) {
        size_t i;
        uint8_t *out = b->rx;
        for (i = 0; i < b->len; i++) out[i] = reply_pos < reply_len ? reply[reply_pos++] : 0xff;
    } else return -EINVAL;
    return 0;
}
static int end(const struct device *dev)
{
    (void)dev; active = 0;
    return fail_end ? -EIO : 0;
}
static int end_with_idle(const struct device *dev, size_t bytes)
{
    if (!active || bytes != 1) return -EIO;
    idle_bytes += (int)bytes;
    return end(dev);
}
static int clock_idle(const struct device *dev, const struct spi_config *cfg, size_t bytes)
{
    (void)dev;
    if (active || cfg->frequency > 400000 || cfg->mode != 0) return -EIO;
    idle_bytes += (int)bytes;
    return 0;
}
int main(void)
{
    const struct spi_driver_api api = { .session_begin = begin, .session_transfer = transfer,
        .session_end = end, .session_end_with_idle = end_with_idle, .clock_idle = clock_idle };
    const struct device bus = { .api = &api, .ready = true };
    const struct sd_spi sd = { .bus = &bus, .config = {
        .frequency = 12000000, .word_size = 8, .mode = 0 } };
    struct sd_spi_card card = {0};
    high_capacity = 1;
    if (sd_spi_card_init(&card, &sd) != 0 || !card.ready || !card.high_capacity ||
        card.spi.config.frequency != 12000000 || highest_frequency != 400000 ||
        tries != 3 || delays != 2 || idle_bytes < 10 || active || last_cmd != 9 ||
        card.sector_count != 1024) return 1;
    tries = 0;
    if (sd_spi_card_init(&card, &card.spi) != 0 || card.spi.config.frequency != 12000000 ||
        !card.ready || active) return 1;
    v1 = 1; high_capacity = 0; tries = 0;
    if (sd_spi_card_init(&card, &sd) != 0 || !card.ready || card.high_capacity ||
        last_cmd != 9 || card.sector_count != 4 || active) return 1;
    v1 = 0; invalid_echo = 1; tries = 0;
    if (sd_spi_card_init(&card, &sd) != -EIO || card.ready || active) return 1;
    invalid_echo = 0; fail_end = 1;
    if (sd_spi_card_init(&card, &sd) != -EIO || card.ready || active) return 1;
    fail_end = 0; stuck = 1; tries = 0;
    if (sd_spi_card_init(&card, &sd) != -ETIMEDOUT || card.ready || active || tries != 1000) return 1;
    puts("SD SPI initialization checks passed");
    return 0;
}
