/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_BOARD_H
#define NOS_TEST_BOARD_H
#include <soc/stm32f4xx.h>
struct dma_config_t {
    DMA_Stream_TypeDef *stream;
    struct { uint32_t DMA_Memory0BaseAddr, DMA_BufferSize; } init_type;
};
struct uart_config_t { struct dma_config_t *dma_config; };
extern struct uart_config_t uart_log_dev;
int dma_config(struct dma_config_t *config);
#endif
