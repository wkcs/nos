/** Copyright (C) 2026 Nick Hu */
#include <drivers/spi_dt.h>
#include <stdio.h>

int main(void)
{
    const struct nos_dt_node *chosen = nos_dt_find_path("/chosen");
    const struct nos_dt_node *node;
    struct spi_dt_resources resources;
    uint32_t phandle, address;

    if (nos_dt_read_u32(chosen, "nos,sd", 0, &phandle) != 0) return 1;
    node = nos_dt_find_phandle(phandle);
    if (!nos_dt_is_compatible(node, "mmc-spi-slot") ||
        spi_dt_resolve(node, &resources) != 0 ||
        !nos_dt_is_compatible(resources.bus, "st,stm32f1-spi") ||
        nos_dt_read_u32(resources.bus, "reg", 0, &address) != 0 || address != 0x40003800 ||
        nos_dt_read_u32(resources.gpio, "reg", 0, &address) != 0 || address != 0x40010800 ||
        resources.gpio_pin != 7 || resources.gpio_flags != 1 ||
        resources.config.frequency != 18000000 || resources.config.mode != 0) return 1;
    puts("ZJ-V3 SD device-tree checks passed");
    return 0;
}
