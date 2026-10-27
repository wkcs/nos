/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARCH_UART_H__
#define __ARCH_UART_H__

#include <kernel/kernel.h>

#include "stm32f4xx.h"
#include "arch_clk.h"
#include "arch_dma.h"
#include "arch_irq.h"
#include "arch_gpio.h"

struct uart_config_t {
    struct clk_config_t *clk_config;
#ifdef CONFIG_UART_DMA
    struct dma_config_t *dma_config;
    uint16_t dma_tx_rx;
#endif
    struct irq_config_t *irq_config;
    uint32_t irq_type;
    struct gpio_config_t (*gpio_config)[];
    uint32_t gpio_group_num;
    USART_TypeDef *uart;
    USART_InitTypeDef init_type;
};

int uart_config(struct uart_config_t *config);
int usart_send(const char *ptr, int len);
#endif


