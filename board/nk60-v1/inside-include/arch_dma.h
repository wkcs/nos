/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARCH_DMA_H__
#define __ARCH_DMA_H__

#include "stm32f4xx.h"

struct dma_config_t {
    struct clk_config_t *clk_config;
    struct irq_config_t *irq_config;
    uint32_t irq_type;
    DMA_Stream_TypeDef *stream;
    DMA_InitTypeDef init_type;
};

int dma_config(struct dma_config_t *config);
void dma_start(DMA_Stream_TypeDef *stream, uint16_t len);

#endif