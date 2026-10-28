/** Copyright (C) 2026 Nick Hu */
#include <drivers/spi.h>
#include <stdio.h>

static int calls;
static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf *buffers, size_t count)
{
    (void)dev;
    calls++;
    if (config->frequency != 1000000 || config->chip_select != 2 ||
        count != 2 || buffers[0].len != 1 || buffers[1].len != 3) {
        return -EINVAL;
    }
    return -EIO;
}

int main(void)
{
    const struct spi_driver_api api = { .transceive = transfer };
    struct device dev = { .api = &api, .ready = true };
    struct spi_config config = { .frequency = 1000000, .chip_select = 2,
                                 .word_size = 8, .mode = 0 };
    unsigned char command = 0x9f;
    unsigned char data[3];
    struct spi_buf buffers[] = {
        { .tx = &command, .len = 1 },
        { .rx = data, .len = sizeof(data) },
    };

    if (spi_transceive(&dev, &config, buffers, 2) != -EIO || calls != 1) {
        fputs("multi-segment transaction or error propagation failed\n", stderr);
        return 1;
    }
    if (spi_transceive(&dev, &config, NULL, 2) != -EINVAL ||
        spi_transceive(&dev, &config, buffers, 0) != -EINVAL) {
        fputs("invalid transaction accepted\n", stderr);
        return 1;
    }
    config.frequency = 0;
    if (spi_transceive(&dev, &config, buffers, 2) != -EINVAL) {
        fputs("invalid frequency accepted\n", stderr);
        return 1;
    }
    config.frequency = 1000000;
    dev.ready = false;
    if (spi_transceive(&dev, &config, buffers, 2) != -ENODEV) {
        fputs("unready controller accepted\n", stderr);
        return 1;
    }
    puts("SPI API checks passed");
    return 0;
}
