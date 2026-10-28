/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_dt.h>

int w25q_dt_resolve(const struct nos_dt_node *node, struct w25q_dt_resources *out)
{
    struct w25q_dt_resources result = { 0 };
    uint32_t chip_select;
    uint32_t phandle;
    uint32_t cells;
    uint32_t index;
    size_t offset = 0;

    if (node == NULL || out == NULL || !nos_dt_is_available(node) ||
        !nos_dt_is_compatible(node, "jedec,spi-nor") || node->parent < 0 ||
        (size_t)node->parent >= nos_dt_node_count) {
        return -EINVAL;
    }
    result.bus = &nos_dt_nodes[node->parent];
    if (!nos_dt_is_available(result.bus) ||
        nos_dt_read_u32(node, "reg", 0, &chip_select) != 0 ||
        chip_select > UINT16_MAX ||
        nos_dt_read_u32(node, "spi-max-frequency", 0, &result.config.frequency) != 0 ||
        result.config.frequency == 0) {
        return -EINVAL;
    }
    for (index = 0; index <= chip_select; index++) {
        if (nos_dt_read_u32(result.bus, "cs-gpios", offset, &phandle) != 0) {
            return -EINVAL;
        }
        result.gpio = nos_dt_find_phandle(phandle);
        if (!nos_dt_is_available(result.gpio) ||
            nos_dt_get_property(result.gpio, "gpio-controller") == NULL ||
            nos_dt_read_u32(result.gpio, "#gpio-cells", 0, &cells) != 0 || cells != 2 ||
            nos_dt_read_u32(result.bus, "cs-gpios", offset + 1, &result.gpio_pin) != 0 ||
            nos_dt_read_u32(result.bus, "cs-gpios", offset + 2, &result.gpio_flags) != 0) {
            return -EINVAL;
        }
        offset += 3;
    }
    result.config.chip_select = (uint16_t)chip_select;
    result.config.word_size = 8;
    if (nos_dt_get_property(node, "spi-cpol") != NULL) {
        result.config.mode |= 2;
    }
    if (nos_dt_get_property(node, "spi-cpha") != NULL) {
        result.config.mode |= 1;
    }
    *out = result;
    return 0;
}
