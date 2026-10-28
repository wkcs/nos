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

const struct nos_dt_node *nos_dt_find_phandle(uint32_t phandle)
{
    size_t i;
    uint32_t value;

    if (phandle == 0 || phandle == UINT32_MAX) {
        return NULL;
    }
    for (i = 0; i < nos_dt_node_count; i++) {
        if (nos_dt_read_u32(&nos_dt_nodes[i], "phandle", 0, &value) == 0 &&
            value == phandle) {
            return &nos_dt_nodes[i];
        }
    }
    return NULL;
}

int nos_dt_is_available(const struct nos_dt_node *node)
{
    const struct nos_dt_property *status;
    size_t depth = 0;

    while (node != NULL && depth++ < nos_dt_node_count) {
        status = nos_dt_get_property(node, "status");
        if (status != NULL &&
            !((status->length == 5 && status->value != NULL &&
               memcmp(status->value, "okay", 5) == 0) ||
              (status->length == 3 && status->value != NULL &&
               memcmp(status->value, "ok", 3) == 0))) {
            return 0;
        }
        if (node->parent == -1) {
            return 1;
        }
        if (node->parent < 0 || (size_t)node->parent >= nos_dt_node_count) {
            return 0;
        }
        node = &nos_dt_nodes[node->parent];
    }
    return 0;
}

int nos_dt_is_compatible(const struct nos_dt_node *node, const char *compatible)
{
    const struct nos_dt_property *prop = nos_dt_get_property(node, "compatible");
    size_t offset = 0;
    size_t length;
    const uint8_t *end;

    if (prop == NULL || prop->value == NULL || compatible == NULL) {
        return 0;
    }
    while (offset < prop->length) {
        end = memchr(prop->value + offset, 0, prop->length - offset);
        if (end == NULL) {
            return 0;
        }
        length = (size_t)(end - (prop->value + offset));
        if (strlen(compatible) == length &&
            memcmp(prop->value + offset, compatible, length) == 0) {
            return 1;
        }
        offset += length + 1;
    }
    return 0;
}
