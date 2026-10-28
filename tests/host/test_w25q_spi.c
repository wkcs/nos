/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_spi.h>
#include <stdio.h>
#include <string.h>

static uint8_t expected_command;
static unsigned int expected_address;
static size_t expected_count, expected_command_length;
static int expected_write, calls, fail;

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf *buffers, size_t count)
{
    const uint8_t *command = buffers[0].tx;
    (void)dev;
    (void)config;
    calls++;
    if (count != expected_count || buffers[0].len != expected_command_length ||
        command[0] != expected_command ||
        (expected_command_length == 4 &&
         (((unsigned int)command[1] << 16) | ((unsigned int)command[2] << 8) |
          command[3]) != expected_address)) {
        return -EINVAL;
    }
    if (fail) return -EIO;
    if (count == 2) {
        if (expected_write) {
            if (buffers[1].tx == NULL || buffers[1].rx != NULL) return -EINVAL;
        } else {
            if (buffers[1].rx == NULL || buffers[1].tx != NULL) return -EINVAL;
            memset(buffers[1].rx, 0xa5, buffers[1].len);
        }
    }
    return 0;
}

int main(void)
{
    const struct spi_driver_api api = { .transceive = transfer };
    const struct device dev = { .api = &api, .ready = true };
    struct w25q_spi flash = { .bus = &dev, .config = { .frequency = 1000000,
                              .word_size = 8, .mode = 0, .chip_select = 0 } };
    uint8_t data[3], status;
    uint16_t id;

    expected_command = 0x03; expected_address = 0x123456;
    expected_count = 2; expected_command_length = 4;
    if (w25q_spi_read(&flash, expected_address, data, sizeof(data)) != 0 ||
        calls != 1 || data[0] != 0xa5 || data[2] != 0xa5) return 1;
    if (w25q_spi_read(&flash, 0xffffff, data, 2) != -EINVAL ||
        w25q_spi_read(&flash, 0, NULL, 1) != -EINVAL ||
        w25q_spi_read(NULL, 0, data, 1) != -EINVAL || calls != 1) return 2;

    expected_command = 0x06; expected_count = 1; expected_command_length = 1;
    if (w25q_spi_write_enable(&flash) != 0) return 3;
    expected_command = 0x05; expected_count = 2;
    if (w25q_spi_read_status(&flash, &status) != 0 || status != 0xa5) return 4;
    expected_command = 0x90; expected_command_length = 4; expected_address = 0;
    if (w25q_spi_read_id(&flash, &id) != 0 || id != 0xa5a5) return 5;
    expected_command = 0x20; expected_count = 1; expected_address = 0x123000;
    if (w25q_spi_erase_sector(&flash, expected_address) != 0 ||
        w25q_spi_erase_sector(&flash, expected_address + 1) != -EINVAL) return 6;
    expected_command = 0x02; expected_count = 2; expected_write = 1;
    expected_address = 0x123100;
    if (w25q_spi_program_page(&flash, expected_address, data, sizeof(data)) != 0 ||
        w25q_spi_program_page(&flash, 0x1231ff, data, 2) != -EINVAL) return 7;
    fail = 1;
    if (w25q_spi_program_page(&flash, expected_address, data, sizeof(data)) != -EIO) return 8;
    puts("W25Q SPI protocol checks passed");
    return 0;
}
