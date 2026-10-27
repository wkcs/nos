/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include "arch_gpio.h"

int gpio_config(struct gpio_config_t *config)
{
    if (config == NULL)
        return 0;

    clk_enable_all(config->clk_config);
    if (config->mux_enable)
        GPIO_PinAFConfig(config->gpio, config->gpio_mux.mux_pin, config->gpio_mux.mux_func);
    GPIO_Init(config->gpio, &config->init_type);

    if (config->irq_config) {
        SYSCFG_EXTILineConfig(config->gpio_port_source, config->gpio_pin_source);
        EXTI_Init(&config->exit_init_type);
        irq_config(config->irq_config);
    }

    return 0;
}
