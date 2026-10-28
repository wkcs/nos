/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_W25Q_DT_H__
#define __NOS_DRIVERS_W25Q_DT_H__

#include <drivers/spi.h>
#include <nos/devicetree.h>

struct w25q_dt_resources {
    const struct nos_dt_node *bus;
    const struct nos_dt_node *gpio;
    struct spi_config config;
    uint32_t gpio_pin;
    uint32_t gpio_flags;
};

int w25q_dt_resolve(const struct nos_dt_node *node, struct w25q_dt_resources *out);

#endif /* __NOS_DRIVERS_W25Q_DT_H__ */
