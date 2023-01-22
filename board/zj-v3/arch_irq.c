/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include "arch_irq.h"

int irq_config(struct irq_config_t *config)
{

    if (config == NULL)
        return 0;

    NVIC_Init(&config->init_type);

    return 0;
}