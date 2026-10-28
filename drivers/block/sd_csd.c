/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>

int sd_spi_csd_sectors(const uint8_t csd[16], uint32_t *sectors)
{
    uint64_t bytes, count;
    uint32_t size, multiplier, block_length;

    if (csd == NULL || sectors == NULL) return -EINVAL;
    switch (csd[0] >> 6) {
    case 0:
        block_length = csd[5] & 0x0fU;
        if (block_length > 11) return -EINVAL;
        size = ((uint32_t)(csd[6] & 3U) << 10) |
               ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        multiplier = ((uint32_t)(csd[9] & 3U) << 1) | (csd[10] >> 7);
        bytes = ((uint64_t)size + 1U) << (multiplier + 2U + block_length);
        if (bytes < 512U || (bytes & 511U) != 0) return -EINVAL;
        count = bytes / 512U;
        break;
    case 1:
        size = ((uint32_t)(csd[7] & 0x3fU) << 16) |
               ((uint32_t)csd[8] << 8) | csd[9];
        count = ((uint64_t)size + 1U) * 1024U;
        break;
    default:
        return -EINVAL;
    }
    if (count > UINT32_MAX) return -EINVAL;
    *sectors = (uint32_t)count;
    return 0;
}
