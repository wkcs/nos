/** Copyright (C) 2026 Nick Hu */
#include <nos/devicetree.h>
#include <kernel/errno.h>
#include <string.h>

const struct nos_dt_node *nos_dt_find_path(const char *path)
{
    size_t i;

    if (path == NULL) {
        return NULL;
    }
    for (i = 0; i < nos_dt_node_count; i++) {
        if (strcmp(nos_dt_nodes[i].path, path) == 0) {
            return &nos_dt_nodes[i];
        }
    }
    return NULL;
}

const struct nos_dt_property *nos_dt_get_property(const struct nos_dt_node *node,
                                                   const char *name)
{
    size_t i;

    if (node == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < node->property_count; i++) {
        if (strcmp(node->properties[i].name, name) == 0) {
            return &node->properties[i];
        }
    }
    return NULL;
}

int nos_dt_read_u32(const struct nos_dt_node *node, const char *name,
                    size_t index, uint32_t *value)
{
    const struct nos_dt_property *prop = nos_dt_get_property(node, name);
    const uint8_t *data;

    if (value == NULL) {
        return -EINVAL;
    }
    if (prop == NULL) {
        return -ENOENT;
    }
    if (prop->value == NULL || prop->length % 4 != 0 || index >= prop->length / 4) {
        return -EINVAL;
    }
    data = prop->value + index * 4;
    *value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
             ((uint32_t)data[2] << 8) | data[3];
    return 0;
}
