/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_spi.h>
#include <stdio.h>

int main(void)
{
    uint8_t csd[16] = {0};
    uint32_t sectors = 0;
    csd[0] = 0x40;
    csd[7] = 0x01; csd[8] = 0x23; csd[9] = 0x45;
    if (sd_spi_csd_sectors(csd, &sectors) != 0 || sectors != (0x12345U + 1) * 1024U) return 1;
    csd[7] = 0x3f; csd[8] = 0xff; csd[9] = 0xff;
    if (sd_spi_csd_sectors(csd, &sectors) != -EINVAL) return 1;
    for (unsigned i = 0; i < sizeof(csd); i++) csd[i] = 0;
    csd[5] = 9;
    csd[6] = 0x02; csd[7] = 0xaa; csd[8] = 0x80;
    csd[9] = 0x02; csd[10] = 0x80;
    if (sd_spi_csd_sectors(csd, &sectors) != 0 || sectors != (0xaaaU + 1) * 128U) return 1;
    csd[0] = 0x80;
    if (sd_spi_csd_sectors(csd, &sectors) != -EINVAL ||
        sd_spi_csd_sectors(NULL, &sectors) != -EINVAL ||
        sd_spi_csd_sectors(csd, NULL) != -EINVAL) return 1;
    puts("SD CSD capacity checks passed");
    return 0;
}
