/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/board.h>
#include <string.h>
#include <stdio.h>

#include "board.h"

#ifdef CONFIG_UART_DMA
static struct clk_config_t uart_log_dev_dma_clk = {
    .clk[CLK_GROUP_AHB] = RCC_AHBPeriph_DMA1
};

static struct irq_config_t uart_log_dev_dma_irq = {
    .init_type = {
        .NVIC_IRQChannel = DMA1_Channel4_IRQn,
        .NVIC_IRQChannelPreemptionPriority = 3,
        .NVIC_IRQChannelSubPriority = 3,
        .NVIC_IRQChannelCmd = ENABLE
    }
};

static struct dma_config_t uart_log_dev_dma = {
    .clk_config = &uart_log_dev_dma_clk,
    .irq_config = &uart_log_dev_dma_irq,
    .irq_type = DMA_IT_TC,
    .ch = DMA1_Channel4,
    .init_type = {
        .DMA_PeripheralBaseAddr = (uint32_t)&USART1->DR,
	    .DMA_MemoryBaseAddr = 0,
	    .DMA_DIR = DMA_DIR_PeripheralDST,
	    .DMA_BufferSize = UART_LOG_DMA_BUF_SIZE,
	    .DMA_PeripheralInc = DMA_PeripheralInc_Disable,
	    .DMA_MemoryInc = DMA_MemoryInc_Enable,
	    .DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte,
	    .DMA_MemoryDataSize = DMA_MemoryDataSize_Byte,
	    .DMA_Mode = DMA_Mode_Normal,
	    .DMA_Priority = DMA_Priority_Low,
	    .DMA_M2M = DMA_M2M_Disable
    }
};
#endif /* CONFIG_UART_DMA */

static struct clk_config_t uart_log_dev_gpio_clk = {
    .clk[CLK_GROUP_APB2] = RCC_APB2Periph_GPIOA
};

static struct gpio_config_t uart_log_dev_gpio[2] = {
    {
        .clk_config = &uart_log_dev_gpio_clk,
        .irq_config = NULL,
        .gpio = GPIOA,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_9,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_AF_PP
        }
    },
    {
        .clk_config = NULL,
        .irq_config = NULL,
        .gpio = GPIOA,
        .init_type = {
            .GPIO_Pin = GPIO_Pin_10,
            .GPIO_Speed = GPIO_Speed_50MHz,
            .GPIO_Mode = GPIO_Mode_IN_FLOATING
        }
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

/**
 * STM32F103 UID 存储在 0x1FFFF7E8 开始的 96 位 (12 字节) 寄存器中
 */
int board_get_chip_uid(uint8_t uid[12])
{
    const volatile uint32_t *src;
    uint32_t tmp[3];

    if (uid == NULL) {
        return -EINVAL;
    }

    src = (const volatile uint32_t *)0x1FFFF7E8UL;
    tmp[0] = src[0];
    tmp[1] = src[1];
    tmp[2] = src[2];
    memcpy(uid, tmp, sizeof(tmp));
    return 0;
}

/**
 * 获取硬件 UID 的十六进制大写字符串表示
 */
int board_get_chip_uid_hex(char *buf, size_t max_len)
{
    uint8_t uid[12] = {0};
    int ret;

    if (buf == NULL || max_len < 25) {
        return -EINVAL;
    }

    ret = board_get_chip_uid(uid);
    if (ret != 0) {
        return ret;
    }

    for (int i = 0; i < 12; i++) {
        snprintf(&buf[i * 2], 3, "%02X", uid[i]);
    }
    buf[24] = '\0';
    return 0;
}
