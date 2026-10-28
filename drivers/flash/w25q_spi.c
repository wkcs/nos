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
