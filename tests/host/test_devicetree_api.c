/** Copyright (C) 2026 Nick Hu */
#include <nos/devicetree.h>
#include <stdio.h>

static const uint8_t compat[] = "vendor,specific\0vendor,generic";
static const uint8_t disabled[] = "disabled";
static const uint8_t handle[] = { 0, 0, 0, 7 };
static const uint8_t broken[] = { 'b', 'a', 'd' };
static const struct nos_dt_property bus_props[] = {
    { "status", disabled, sizeof(disabled) },
    { "phandle", handle, sizeof(handle) },
};
static const struct nos_dt_property child_props[] = {
    { "compatible", compat, sizeof(compat) },
};
static const struct nos_dt_property bad_props[] = {
    { "compatible", broken, sizeof(broken) },
};
const struct nos_dt_node nos_dt_nodes[] = {
    { "/", NULL, 0, -1 },
    { "/bus", bus_props, 2, 0 },
    { "/bus/device", child_props, 1, 1 },
    { "/bad", bad_props, 1, 0 },
};
const size_t nos_dt_node_count = 4;

int main(void)
{
    const struct nos_dt_node *node = nos_dt_find_path("/bus/device");
    if (!node || nos_dt_is_available(node) ||
        !nos_dt_is_compatible(node, "vendor,generic") ||
        nos_dt_is_compatible(node, "vendor,missing") ||
        nos_dt_is_compatible(&nos_dt_nodes[3], "bad") ||
        nos_dt_find_phandle(7) != &nos_dt_nodes[1] ||
        nos_dt_find_phandle(0) != NULL || nos_dt_find_phandle(8) != NULL) {
        fputs("device tree matching or status failed\n", stderr);
        return 1;
    }
    puts("device tree API checks passed");
    return 0;
}
