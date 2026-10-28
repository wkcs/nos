/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DEVICETREE_H__
#define __NOS_DEVICETREE_H__

#include <stddef.h>
#include <stdint.h>

struct nos_dt_property {
    const char *name;
    const uint8_t *value;
    size_t length;
};

struct nos_dt_node {
    const char *path;
    const struct nos_dt_property *properties;
    size_t property_count;
    int32_t parent;
};

extern const struct nos_dt_node nos_dt_nodes[];
extern const size_t nos_dt_node_count;

const struct nos_dt_node *nos_dt_find_path(const char *path);
const struct nos_dt_node *nos_dt_find_phandle(uint32_t phandle);
int nos_dt_is_available(const struct nos_dt_node *node);
int nos_dt_is_compatible(const struct nos_dt_node *node, const char *compatible);
const struct nos_dt_property *nos_dt_get_property(const struct nos_dt_node *node,
                                                   const char *name);
int nos_dt_read_u32(const struct nos_dt_node *node, const char *name,
                    size_t index, uint32_t *value);

#endif /* __NOS_DEVICETREE_H__ */
