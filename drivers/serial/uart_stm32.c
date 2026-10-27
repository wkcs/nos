/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <drivers/serial.h>
#include <kernel/device.h>
#ifdef CONFIG_SOC_STM32F4
#include <soc/stm32f4xx.h>
#else
#include <soc/stm32f1xx.h>
#endif

struct uart_stm32_config {
    USART_TypeDef *regs;
    uint32_t clock;
    uint32_t gpio_clock;
    GPIO_TypeDef *gpio;
    uint16_t tx_pin;
    uint16_t rx_pin;
#ifdef CONFIG_SOC_STM32F4
    uint8_t tx_pin_source;
    uint8_t rx_pin_source;
    uint8_t gpio_af;
#endif
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

#ifdef CONFIG_SOC_STM32F4
    RCC_AHB1PeriphClockCmd(config->gpio_clock, ENABLE);
    RCC_APB2PeriphClockCmd(config->clock, ENABLE);

    GPIO_PinAFConfig(config->gpio, config->tx_pin_source, config->gpio_af);
    GPIO_PinAFConfig(config->gpio, config->rx_pin_source, config->gpio_af);

    gpio_init.GPIO_Pin = config->tx_pin | config->rx_pin;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(config->gpio, &gpio_init);
#else
    RCC_APB2PeriphClockCmd(config->gpio_clock | config->clock, ENABLE);

    gpio_init.GPIO_Pin = config->tx_pin;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(config->gpio, &gpio_init);

    gpio_init.GPIO_Pin = config->rx_pin;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(config->gpio, &gpio_init);
#endif

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

static const struct uart_stm32_config qemu_stm32_uart1_config = {
    .regs = USART1,
    .clock = RCC_APB2Periph_USART1,
#ifdef CONFIG_SOC_STM32F4
    .gpio_clock = RCC_AHB1Periph_GPIOA,
#else
    .gpio_clock = RCC_APB2Periph_GPIOA,
#endif
    .gpio = GPIOA,
    .tx_pin = GPIO_Pin_9,
    .rx_pin = GPIO_Pin_10,
#ifdef CONFIG_SOC_STM32F4
    .tx_pin_source = GPIO_PinSource9,
    .rx_pin_source = GPIO_PinSource10,
    .gpio_af = GPIO_AF_USART1,
#endif
    .baud_rate = 115200,
};

DEVICE_DEFINE(qemu_stm32_uart1, "UART1", uart_stm32_init, &qemu_stm32_uart1_config,
              NULL, &uart_stm32_api, PRE_KERNEL_1);
