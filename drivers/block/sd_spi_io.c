/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>

#define SD_BLOCK_SIZE 512U

static uint16_t sd_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000U) ? 0x1021U : 0));
        }
    }
    return crc;
}

static int sd_transfer(struct spi_session *session, const void *tx, void *rx, size_t length)
{
    const struct spi_buf buffer = { .tx = tx, .rx = rx, .len = length };
    return spi_session_transfer(session, &buffer, 1);
}

static int sd_block_address(const struct sd_spi_card *card, uint32_t lba, uint32_t *address)
{
    if (!card->high_capacity && lba > UINT32_MAX / SD_BLOCK_SIZE) return -EINVAL;
    *address = card->high_capacity ? lba : lba * SD_BLOCK_SIZE;
    return 0;
}

static int sd_end(const struct sd_spi_card *card, struct spi_session *session, int ret)
{
    int cleanup;

    if (session->active) {
        cleanup = spi_session_end_with_idle(session, 1);
    } else {
        cleanup = spi_clock_idle(card->spi.bus, &card->spi.config, 1);
    }
    return ret != 0 ? ret : cleanup;
}

/* Limit polling to approximately one second of clocks, even if the card is
 * removed or holds the line indefinitely. */
static uint32_t sd_poll_limit(const struct sd_spi_card *card)
{
    uint32_t limit = card->spi.config.frequency / 8U;
    return limit == 0 ? 1 : limit;
}

static int sd_read_data(struct sd_spi_card *card, uint8_t command, uint32_t address,
                        void *buffer, size_t length)
{
    struct spi_session session = {0};
    uint32_t remaining;
    uint8_t response, token, crc_bytes[2];
    uint16_t crc;
    int ret;

    ret = sd_spi_begin_command(&card->spi, &session, command, address, &response);
    if (ret == 0 && response != 0) ret = -EIO;
    remaining = sd_poll_limit(card);
    while (ret == 0 && remaining-- != 0) {
        ret = sd_transfer(&session, NULL, &token, 1);
        if (ret != 0 || token != 0xffU) break;
    }
    if (ret == 0 && token == 0xffU) ret = -ETIMEDOUT;
    if (ret == 0 && token != 0xfeU) ret = -EIO;
    if (ret == 0) ret = sd_transfer(&session, NULL, buffer, length);
    if (ret == 0) ret = sd_transfer(&session, NULL, crc_bytes, sizeof(crc_bytes));
    if (ret == 0) {
        crc = sd_crc16(buffer, length);
        if (crc_bytes[0] != (uint8_t)(crc >> 8) || crc_bytes[1] != (uint8_t)crc) ret = -EIO;
    }
    return sd_end(card, &session, ret);
}

int sd_spi_read_block(struct sd_spi_card *card, uint32_t lba, void *buffer)
{
    uint32_t address;
    int ret;

    if (card == NULL || buffer == NULL) return -EINVAL;
    if (!card->ready) return -ENODEV;
    if (lba >= card->sector_count) return -EINVAL;
    ret = sd_block_address(card, lba, &address);
    if (ret != 0) return ret;
    return sd_read_data(card, 17, address, buffer, SD_BLOCK_SIZE);
}

int sd_spi_read_capacity(struct sd_spi_card *card, uint32_t *sectors)
{
    uint8_t csd[16];
    int ret;

    if (card == NULL || sectors == NULL) return -EINVAL;
    if (!card->ready) return -ENODEV;
    ret = sd_read_data(card, 9, 0, csd, sizeof(csd));
    return ret != 0 ? ret : sd_spi_csd_sectors(csd, sectors);
}

int sd_spi_write_block(struct sd_spi_card *card, uint32_t lba, const void *buffer)
{
    struct spi_session session = {0};
    uint32_t address, remaining;
    uint8_t response, token = 0xfeU, crc_bytes[2];
    uint16_t crc;
    int ret;

    if (card == NULL || buffer == NULL) return -EINVAL;
    if (!card->ready) return -ENODEV;
    if (lba >= card->sector_count) return -EINVAL;
    ret = sd_block_address(card, lba, &address);
    if (ret != 0) return ret;
    ret = sd_spi_begin_command(&card->spi, &session, 24, address, &response);
    if (ret == 0 && response != 0) ret = -EIO;
    if (ret == 0) ret = sd_transfer(&session, &token, NULL, 1);
    if (ret == 0) ret = sd_transfer(&session, buffer, NULL, SD_BLOCK_SIZE);
    crc = sd_crc16(buffer, SD_BLOCK_SIZE);
    crc_bytes[0] = (uint8_t)(crc >> 8);
    crc_bytes[1] = (uint8_t)crc;
    if (ret == 0) ret = sd_transfer(&session, crc_bytes, NULL, sizeof(crc_bytes));
    if (ret == 0) ret = sd_transfer(&session, NULL, &response, 1);
    if (ret == 0 && (response & 0x1fU) != 0x05U) ret = -EIO;
    remaining = sd_poll_limit(card);
    while (ret == 0 && remaining-- != 0) {
        ret = sd_transfer(&session, NULL, &response, 1);
        if (ret != 0 || response == 0xffU) break;
    }
    if (ret == 0 && response != 0xffU) ret = -ETIMEDOUT;
    return sd_end(card, &session, ret);
}
