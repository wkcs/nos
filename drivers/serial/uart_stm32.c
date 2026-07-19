/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <drivers/serial.h>
#include <kernel/device.h>
#include <soc/stm32f1xx.h>

struct uart_stm32_config {
    USART_TypeDef *regs;
    uint32_t clock;
    uint32_t gpio_clock;
    GPIO_TypeDef *gpio;
    uint16_t tx_pin;
    uint16_t rx_pin;
    uint32_t baud_rate;
};

static int uart_stm32_poll_out(const struct device *dev, unsigned char out)
{
    const struct uart_stm32_config *config = dev->config;

    while (USART_GetFlagStatus(config->regs, USART_FLAG_TC) != SET) {
    }
    USART_SendData(config->regs, out);

    return 0;
}

static int uart_stm32_poll_in(const struct device *dev, unsigned char *in)
{
    const struct uart_stm32_config *config = dev->config;

    if (USART_GetFlagStatus(config->regs, USART_FLAG_RXNE) == RESET) {
        return -EAGAIN;
    }

    *in = (unsigned char)USART_ReceiveData(config->regs);
    return 0;
}

static int uart_stm32_init(struct device *dev)
{
    const struct uart_stm32_config *config = dev->config;
    GPIO_InitTypeDef gpio_init;
    USART_InitTypeDef uart_init;

    RCC_APB2PeriphClockCmd(config->gpio_clock | config->clock, ENABLE);

    gpio_init.GPIO_Pin = config->tx_pin;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(config->gpio, &gpio_init);

    gpio_init.GPIO_Pin = config->rx_pin;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(config->gpio, &gpio_init);

    uart_init.USART_BaudRate = config->baud_rate;
    uart_init.USART_WordLength = USART_WordLength_8b;
    uart_init.USART_StopBits = USART_StopBits_1;
    uart_init.USART_Parity = USART_Parity_No;
    uart_init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart_init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(config->regs, &uart_init);
    USART_Cmd(config->regs, ENABLE);

    return 0;
}

static const struct uart_driver_api uart_stm32_api = {
    .poll_out = uart_stm32_poll_out,
    .poll_in = uart_stm32_poll_in,
};

/*
 * qemu-stm32 exposes its console on USART1's default PA9/PA10 pins.
 * Keep this golden-board configuration here until board device metadata exists.
 */
static const struct uart_stm32_config qemu_stm32_uart1_config = {
    .regs = USART1,
    .clock = RCC_APB2Periph_USART1,
    .gpio_clock = RCC_APB2Periph_GPIOA,
    .gpio = GPIOA,
    .tx_pin = GPIO_Pin_9,
    .rx_pin = GPIO_Pin_10,
    .baud_rate = 115200,
};

DEVICE_DEFINE(qemu_stm32_uart1, "UART1", uart_stm32_init, &qemu_stm32_uart1_config,
              NULL, &uart_stm32_api, PRE_KERNEL_1);
