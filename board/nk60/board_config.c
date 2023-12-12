/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>

#include "board.h"

#ifdef CONFIG_UART_DMA
static struct clk_config_t uart_log_dev_dma_clk = {
    .clk[CLK_GROUP_AHB1] = RCC_AHB1Periph_DMA2
};

static struct irq_config_t uart_log_dev_dma_irq = {
    .init_type = {
        .NVIC_IRQChannel = DMA2_Stream7_IRQn,
        .NVIC_IRQChannelPreemptionPriority = 3,
        .NVIC_IRQChannelSubPriority = 3,
        .NVIC_IRQChannelCmd = ENABLE
    }
};

static struct dma_config_t uart_log_dev_dma = {
    .clk_config = &uart_log_dev_dma_clk,
    .irq_config = &uart_log_dev_dma_irq,
    .irq_type = DMA_IT_TC,
    .stream = DMA2_Stream7,
    .init_type = {
        .DMA_Channel = DMA_Channel_4,
        .DMA_PeripheralBaseAddr = (uint32_t)&USART1->DR,
        .DMA_Memory0BaseAddr = 0,
        .DMA_DIR = DMA_DIR_MemoryToPeripheral,
        .DMA_BufferSize = UART_LOG_DMA_BUF_SIZE,
        .DMA_PeripheralInc = DMA_PeripheralInc_Disable,
        .DMA_MemoryInc = DMA_MemoryInc_Enable,
        .DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte,
        .DMA_MemoryDataSize = DMA_MemoryDataSize_Byte,
        .DMA_Mode = DMA_Mode_Normal,
        .DMA_Priority = DMA_Priority_Medium,
        .DMA_FIFOMode = DMA_FIFOMode_Disable,
        .DMA_FIFOThreshold = DMA_FIFOThreshold_Full,
        .DMA_MemoryBurst = DMA_MemoryBurst_Single,
        .DMA_PeripheralBurst = DMA_PeripheralBurst_Single,
    }
};
#endif /* CONFIG_UART_DMA */

static struct clk_config_t uart_log_dev_gpio_clk = {
    .clk[CLK_GROUP_AHB1] = RCC_AHB1Periph_GPIOA
};

static struct clk_config_t cc1_gpio_clk = {
    .clk[CLK_GROUP_AHB1] = RCC_AHB1Periph_GPIOC
};

static struct clk_config_t cc2_gpio_clk = {
    .clk[CLK_GROUP_AHB1] = RCC_AHB1Periph_GPIOD
};

static struct gpio_config_t uart_log_dev_gpio[2] = {
    {
        .clk_config = &uart_log_dev_gpio_clk,
        .irq_config = NULL,
        .gpio = GPIOA,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_9,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_AF,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_UP,
        },
        .mux_enable = true,
        .gpio_mux = {
            .mux_func = GPIO_AF_USART1,
            .mux_pin = GPIO_PinSource9,
        },
    },
    {
        .clk_config = NULL,
        .irq_config = NULL,
        .gpio = GPIOA,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_10,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_AF,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_UP,
        },
        .mux_enable = true,
        .gpio_mux = {
            .mux_func = GPIO_AF_USART1,
            .mux_pin = GPIO_PinSource10,
        },
    }
};

struct gpio_config_t cc_gpio[2] = {
    {
        .clk_config = &cc1_gpio_clk,
        .irq_config = NULL,
        .gpio = GPIOC,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_15,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_IN,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_NOPULL,
        },
        .mux_enable = false,
    },
    {
        .clk_config = &cc2_gpio_clk,
        .irq_config = NULL,
        .gpio = GPIOD,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_2,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_IN,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_NOPULL,
        },
        .mux_enable = false,
    }
};

static struct clk_config_t uart_log_dev_clk = {
    .clk[CLK_GROUP_APB2] = RCC_APB2Periph_USART1
};

static struct irq_config_t uart_log_dev_irq = {
    .init_type = {
        .NVIC_IRQChannel = USART1_IRQn,
        .NVIC_IRQChannelPreemptionPriority = 3,
        .NVIC_IRQChannelSubPriority = 0,
        .NVIC_IRQChannelCmd = ENABLE
    }
};

struct uart_config_t uart_log_dev = {
    .clk_config = &uart_log_dev_clk,
#ifdef CONFIG_UART_DMA
    .dma_config = &uart_log_dev_dma,
    .dma_tx_rx = USART_DMAReq_Tx,
#endif
    .irq_config = &uart_log_dev_irq,
    .irq_type = USART_IT_RXNE,
    .gpio_config = &uart_log_dev_gpio,
    .gpio_group_num = 2,
    .uart = USART1,
    .init_type = {
        .USART_BaudRate = 115200,
        .USART_WordLength = USART_WordLength_8b,
        .USART_StopBits = USART_StopBits_1,
        .USART_Parity = USART_Parity_No,
        .USART_HardwareFlowControl = USART_HardwareFlowControl_None,
        .USART_Mode = USART_Mode_Rx | USART_Mode_Tx
    }
};
