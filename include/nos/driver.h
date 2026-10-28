/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVER_H__
#define __NOS_DRIVER_H__

#include <kernel/device.h>
#include <nos/devicetree.h>

struct nos_dt_match {
    const char *compatible;
    const void *data;
};

struct nos_dt_driver {
    const char *name;
    const struct nos_dt_match *matches;
    size_t match_count;
    int (*probe)(struct device *dev, const struct nos_dt_node *node,
                 const void *match_data);
};

/* dev must be initialized and remain alive throughout probe. A failed probe
 * must release its own resources; -EAGAIN may be retried by the caller. */
int nos_dt_device_bind(struct device *dev, const struct nos_dt_node *node,
                       const struct nos_dt_driver *driver);

#endif /* __NOS_DRIVER_H__ */
