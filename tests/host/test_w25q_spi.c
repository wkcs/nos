/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_spi.h>
#include <stdio.h>
#include <string.h>

static int expected_command;
static unsigned int expected_address;
static int calls;

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf *buffers, size_t count)
{
    const uint8_t *command = buffers[0].tx;
    (void)dev;
    (void)config;
    calls++;
    if (count != 2 || buffers[0].len != 4 || command[0] != expected_command ||
        (((unsigned int)command[1] << 16) | ((unsigned int)command[2] << 8) |
         command[3]) != expected_address || buffers[1].rx == NULL ||
        buffers[1].tx != NULL) {
        return -EINVAL;
    }
    memset(buffers[1].rx, 0xa5, buffers[1].len);
    return 0;
}

int main(void)
{
    const struct spi_driver_api api = { .transceive = transfer };
    const struct device dev = { .api = &api, .ready = true };
    struct w25q_spi flash = { .bus = &dev, .config = { .frequency = 1000000,
                              .word_size = 8, .mode = 0, .chip_select = 0 } };
    uint8_t data[3];

    expected_command = 0x03;
    expected_address = 0x123456;
    if (w25q_spi_read(&flash, expected_address, data, sizeof(data)) != 0 ||
        calls != 1 || data[0] != 0xa5 || data[2] != 0xa5) {
        fputs("read transaction failed\n", stderr);
        return 1;
    }
    if (w25q_spi_read(&flash, 0xffffff, data, 2) != -EINVAL ||
        w25q_spi_read(&flash, 0, NULL, 1) != -EINVAL ||
        w25q_spi_read(NULL, 0, data, 1) != -EINVAL || calls != 1) {
        fputs("invalid read accepted\n", stderr);
        return 1;
    }
    puts("W25Q SPI protocol checks passed");
    return 0;
}
