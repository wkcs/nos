/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "stm32f10x.h"
#include "arch_clk.h"
#include "arch_dma.h"
#include "arch_gpio.h"
#include "arch_irq.h"
#include "arch_uart.h"

#define UART_LOG_DMA_BUF_SIZE 512

extern struct uart_config_t uart_log_dev;

void board_mm_init(void);

#endif /* __BOARD_H__ */