/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_dt.h>
#include <kernel/errno.h>
#include <stdio.h>

static const uint8_t chosen_flash[] = { 0, 0, 0, 3 };
static uint8_t cs[] = { 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1 };
static const uint8_t gpio_cells[] = { 0, 0, 0, 2 };
static const uint8_t handle2[] = { 0, 0, 0, 2 };
static const uint8_t handle3[] = { 0, 0, 0, 3 };
static const uint8_t zero[] = { 0, 0, 0, 0 };
static const uint8_t frequency[] = { 0x01, 0x40, 0x6f, 0x40 };
static const uint8_t compatible[] = "jedec,spi-nor";
static const struct nos_dt_property chosen_props[] = { { "nos,flash", chosen_flash, 4 } };
static const struct nos_dt_property bus_props[] = { { "cs-gpios", cs, 12 } };
static const struct nos_dt_property gpio_props[] = {
    { "phandle", handle2, 4 }, { "#gpio-cells", gpio_cells, 4 },
    { "gpio-controller", NULL, 0 },
};
static const struct nos_dt_property flash_props[] = {
    { "compatible", compatible, sizeof(compatible) }, { "phandle", handle3, 4 },
    { "reg", zero, 4 }, { "spi-max-frequency", frequency, 4 },
    { "spi-cpol", NULL, 0 }, { "spi-cpha", NULL, 0 },
};
const struct nos_dt_node nos_dt_nodes[] = {
    { "/", NULL, 0, -1 }, { "/chosen", chosen_props, 1, 0 },
    { "/spi", bus_props, 1, 0 }, { "/gpio", gpio_props, 3, 0 },
    { "/spi/flash@0", flash_props, 6, 2 },
};
const size_t nos_dt_node_count = 5;

int main(void)
{
    struct w25q_dt_resources resources;
    int ret = w25q_dt_resolve(&nos_dt_nodes[4], &resources);
    if (ret != 0 || resources.bus != &nos_dt_nodes[2] ||
        resources.gpio != &nos_dt_nodes[3] || resources.gpio_pin != 2 ||
        resources.gpio_flags != 1 || resources.config.chip_select != 0 ||
        resources.config.mode != 3 || resources.config.frequency != 21000000) {
        fputs("W25Q device-tree resources did not resolve\n", stderr);
        return 1;
    }
    cs[3] = 9;
    if (w25q_dt_resolve(&nos_dt_nodes[4], &resources) >= 0) {
        fputs("invalid GPIO phandle accepted\n", stderr);
        return 1;
    }
    puts("W25Q device-tree checks passed");
    return 0;
}
