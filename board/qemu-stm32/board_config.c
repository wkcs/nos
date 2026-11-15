/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/errno.h>
#include <kernel/board.h>
#include <string.h>
#include <stdio.h>

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
 * 获取 QEMU-STM32 模拟硬件唯一标识符 (UID)
 *
 * 优先尝试从 STM32 UID 寄存器读取；若在 QEMU 虚拟环境下读出全 0，
 * 则使用固定的 QEMU-STM32 模拟 UID，确保序列号非空且唯一稳定。
 */
int board_get_chip_uid(uint8_t uid[12])
{
    const volatile uint32_t *src;
    uint32_t tmp[3];
    static const uint8_t qemu_default_uid[12] = {
        'Q', 'E', 'M', 'U', 'S', 'T', 'M', '3', '2', '0', '0', '1'
    };

    if (uid == NULL) {
        return -EINVAL;
    }

#if defined(CONFIG_SOC_STM32F4)
    src = (const volatile uint32_t *)0x1FFF7A10UL;
#else
    src = (const volatile uint32_t *)0x1FFFF7E8UL;
#endif
    tmp[0] = src[0];
    tmp[1] = src[1];
    tmp[2] = src[2];

    /* 若 QEMU 未模拟真实 UID 寄存器（读出全 0），采用模拟默认 UID */
    if (tmp[0] == 0 && tmp[1] == 0 && tmp[2] == 0) {
        memcpy(uid, qemu_default_uid, sizeof(qemu_default_uid));
    } else {
        memcpy(uid, tmp, sizeof(tmp));
    }
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

