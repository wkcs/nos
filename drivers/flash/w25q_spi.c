/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_spi.h>

#define W25Q_READ_DATA 0x03U
#define W25Q_ADDRESS_SPACE 0x1000000U

int w25q_spi_read(const struct w25q_spi *flash, uint32_t address,
                  void *buffer, size_t length)
{
    uint8_t command[4];
    struct spi_buf transfers[2];

    if (flash == NULL || buffer == NULL || length == 0 ||
        address >= W25Q_ADDRESS_SPACE || length > W25Q_ADDRESS_SPACE - address) {
        return -EINVAL;
    }
    command[0] = W25Q_READ_DATA;
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
    transfers[0] = (struct spi_buf) { .tx = command, .len = sizeof(command) };
    transfers[1] = (struct spi_buf) { .rx = buffer, .len = length };
    return spi_transceive(flash->bus, &flash->config, transfers, 2);
}

int w25q_spi_write_enable(const struct w25q_spi *flash)
{
    const uint8_t command = 0x06U;
    const struct spi_buf buffer = { .tx = &command, .len = 1 };

    if (flash == NULL) {
        return -EINVAL;
    }
    return spi_transceive(flash->bus, &flash->config, &buffer, 1);
}

int w25q_spi_read_status(const struct w25q_spi *flash, uint8_t *status)
{
    const uint8_t command = 0x05U;
    const struct spi_buf buffers[] = {
        { .tx = &command, .len = 1 },
        { .rx = status, .len = 1 },
    };

    if (flash == NULL || status == NULL) {
        return -EINVAL;
    }
    return spi_transceive(flash->bus, &flash->config, buffers, 2);
}

int w25q_spi_read_id(const struct w25q_spi *flash, uint16_t *id)
{
    const uint8_t command[] = { 0x90U, 0, 0, 0 };
    uint8_t value[2];
    const struct spi_buf buffers[] = {
        { .tx = command, .len = sizeof(command) },
        { .rx = value, .len = sizeof(value) },
    };
    int ret;

    if (flash == NULL || id == NULL) {
        return -EINVAL;
    }
    ret = spi_transceive(flash->bus, &flash->config, buffers, 2);
    if (ret == 0) {
        *id = ((uint16_t)value[0] << 8) | value[1];
    }
    return ret;
}

int w25q_spi_erase_sector(const struct w25q_spi *flash, uint32_t address)
{
    const uint8_t command[] = { 0x20U, (uint8_t)(address >> 16),
                                (uint8_t)(address >> 8), (uint8_t)address };
    const struct spi_buf buffer = { .tx = command, .len = sizeof(command) };

    if (flash == NULL || address >= W25Q_ADDRESS_SPACE || (address & 0xfffU) != 0) {
        return -EINVAL;
    }
    return spi_transceive(flash->bus, &flash->config, &buffer, 1);
}

int w25q_spi_program_page(const struct w25q_spi *flash, uint32_t address,
                          const void *buffer, size_t length)
{
    const uint8_t command[] = { 0x02U, (uint8_t)(address >> 16),
                                (uint8_t)(address >> 8), (uint8_t)address };
    const struct spi_buf buffers[] = {
        { .tx = command, .len = sizeof(command) },
        { .tx = buffer, .len = length },
    };

    if (flash == NULL || buffer == NULL || length == 0 ||
        address >= W25Q_ADDRESS_SPACE || length > 256U - (address & 0xffU)) {
        return -EINVAL;
    }
    return spi_transceive(flash->bus, &flash->config, buffers, 2);
}
