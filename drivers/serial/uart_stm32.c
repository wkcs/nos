/**
 * Copyright (C) 2026 Nick Hu
 *
 * STM32 USART1 driver with bounded IRQ-driven RX/TX queues.
 */

#include <drivers/serial.h>
#include <kernel/device.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <lib/kfifo.h>
#include <nos/ipc.h>
#if defined(CONFIG_DEVICE_TREE) && defined(CONFIG_BOARD_QEMU_STM32)
#include <nos/devicetree.h>
#endif
#ifdef CONFIG_UART_DMA
#include "arch_uart.h"
extern struct uart_config_t uart_log_dev;
#endif
#ifdef CONFIG_SOC_STM32F4
#include <soc/stm32f4xx.h>
#else
#include <soc/stm32f1xx.h>
#endif

#define NOS_UART_BUFFERED_DRIVER 1
#ifdef CONFIG_UART_DMA
#define NOS_UART_DMA_DRIVER 1
#endif
#define UART_NOTIFY_READY 1ULL

#ifndef CONFIG_UART_RX_BUFFER_SIZE
#define CONFIG_UART_RX_BUFFER_SIZE 128
#endif
#ifndef CONFIG_UART_TX_BUFFER_SIZE
#define CONFIG_UART_TX_BUFFER_SIZE 128
#endif
#ifndef CONFIG_UART_POLL_TIMEOUT
#define CONFIG_UART_POLL_TIMEOUT 1000000
#endif
#ifndef CONFIG_UART_DMA_BUFFER_SIZE
#define CONFIG_UART_DMA_BUFFER_SIZE 128
#endif
#ifndef CONFIG_UART_DMA_STOP_TIMEOUT
#define CONFIG_UART_DMA_STOP_TIMEOUT 4096
#endif
#ifndef CONFIG_UART_BAUD_RATE
#define CONFIG_UART_BAUD_RATE 115200
#endif

_Static_assert(CONFIG_UART_RX_BUFFER_SIZE >= 2 &&
               (CONFIG_UART_RX_BUFFER_SIZE & (CONFIG_UART_RX_BUFFER_SIZE - 1)) == 0,
               "UART RX buffer size must be a power of two");
_Static_assert(CONFIG_UART_TX_BUFFER_SIZE >= 2 &&
               (CONFIG_UART_TX_BUFFER_SIZE & (CONFIG_UART_TX_BUFFER_SIZE - 1)) == 0,
               "UART TX buffer size must be a power of two");

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
    uint32_t irq;
};

struct uart_stm32_data {
    spinlock_t lock;
    struct kfifo rx_fifo;
    struct kfifo tx_fifo;
    struct nos_notification rx_ready;
    struct nos_notification tx_ready;
    unsigned char rx_buf[CONFIG_UART_RX_BUFFER_SIZE];
    unsigned char tx_buf[CONFIG_UART_TX_BUFFER_SIZE];
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t dma_errors;
    bool panic;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t flow_ctrl;
#ifdef CONFIG_UART_DMA
    unsigned char dma_buf[CONFIG_UART_DMA_BUFFER_SIZE];
    unsigned int dma_len;
    unsigned int dma_pos;
    bool dma_active;
    bool dma_faulted;
    bool dma_stalled;
#endif
};

static struct uart_stm32_data uart1_data;

static bool uart_in_interrupt(void)
{
#ifdef CONFIG_SOC_STM32F1
    uint32_t ipsr;

    /* The STM32F1 CMSIS headers predate __get_IPSR(). */
    __asm__ volatile ("mrs %0, ipsr" : "=r" (ipsr));
    return ipsr != 0;
#else
    return __get_IPSR() != 0;
#endif
}

static addr_t uart_lock(struct uart_stm32_data *data)
{
    addr_t flags = disable_irq_save();
    spin_lock(&data->lock);
    return flags;
}

static void uart_unlock(struct uart_stm32_data *data, addr_t flags)
{
    spin_unlock(&data->lock);
    enable_irq_save(flags);
}

#ifdef CONFIG_UART_DMA
static int uart_stm32_dma_stop(void)
{
#ifdef CONFIG_SOC_STM32F4
    uint32_t remaining = CONFIG_UART_DMA_STOP_TIMEOUT;

    DMA_Cmd(uart_log_dev.dma_config->stream, DISABLE);
    while (DMA_GetCmdStatus(uart_log_dev.dma_config->stream) != DISABLE) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
#else
    DMA_Cmd(uart_log_dev.dma_config->ch, DISABLE);
#endif
    return 0;
}
#endif

static void uart_stm32_kick_locked(const struct device *dev)
{
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;

#ifdef CONFIG_UART_DMA
    if (data->dma_stalled) {
        return;
    }
    if (!data->dma_faulted) {
        if (!data->dma_active) {
            unsigned int len = kfifo_out(&data->tx_fifo, data->dma_buf,
                                         sizeof(data->dma_buf));
            if (len != 0) {
                data->dma_len = len;
                data->dma_pos = 0;
                data->dma_active = true;
                if (uart_stm32_dma_stop() < 0) {
                    data->dma_stalled = true;
                    data->dma_errors++;
                    USART_DMACmd(config->regs, USART_DMAReq_Tx, DISABLE);
                    return;
                }
#ifdef CONFIG_SOC_STM32F4
                DMA_SetCurrDataCounter(uart_log_dev.dma_config->stream, len);
                DMA_Cmd(uart_log_dev.dma_config->stream, ENABLE);
#else
                DMA_SetCurrDataCounter(uart_log_dev.dma_config->ch, len);
                DMA_Cmd(uart_log_dev.dma_config->ch, ENABLE);
#endif
            }
        }
        return;
    }
#endif
    if (kfifo_used(&data->tx_fifo) != 0
#ifdef CONFIG_UART_DMA
        || data->dma_pos < data->dma_len
#endif
    ) {
        USART_ITConfig(config->regs, USART_IT_TXE, ENABLE);
    }
}

static int uart_stm32_poll_out(const struct device *dev, unsigned char out)
{
    const struct uart_stm32_config *config = dev->config;
    uint32_t remaining = CONFIG_UART_POLL_TIMEOUT;

    while (USART_GetFlagStatus(config->regs, USART_FLAG_TXE) != SET) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
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

static ssize_t uart_stm32_poll_write(const struct device *dev,
                                     const unsigned char *buf, size_t len)
{
    size_t written;

    for (written = 0; written < len; written++) {
        int rc = uart_stm32_poll_out(dev, buf[written]);
        if (rc < 0) {
            return written != 0 ? (ssize_t)written : rc;
        }
    }
    return (ssize_t)written;
}

static ssize_t uart_stm32_write(const struct device *dev,
                                const unsigned char *buf, size_t len)
{
    struct uart_stm32_data *data = dev->data;
    size_t written = 0;

    if (len == 0) {
        return 0;
    }
#ifdef CONFIG_UART_DMA
    if (data->dma_stalled && !data->panic) {
        return -EIO;
    }
#endif
    if (!kernel_running || data->panic) {
        return uart_stm32_poll_write(dev, buf, len);
    }
    while (written < len) {
        uint64_t bits;
        unsigned int accepted;
        addr_t flags = uart_lock(data);
        accepted = kfifo_in(&data->tx_fifo, buf + written, (unsigned int)(len - written));
        if (accepted != 0) {
            uart_stm32_kick_locked(dev);
        }
        uart_unlock(data, flags);
        written += accepted;
        if (written == len) {
            break;
        }
        if (uart_in_interrupt() || flags != 0) {
            return written != 0 ? (ssize_t)written : -EAGAIN;
        }
        int rc = nos_notify_wait_obj(&data->tx_ready, &bits,
                                     NOS_IPC_TIMEOUT_INFINITE);
        if (rc < 0) {
            return written != 0 ? (ssize_t)written : rc;
        }
    }
    return (ssize_t)written;
}

static ssize_t uart_stm32_read(const struct device *dev, unsigned char *buf,
                               size_t len, bool wait)
{
    struct uart_stm32_data *data = dev->data;

    if (len == 0) {
        return 0;
    }
    for (;;) {
        unsigned int received;
        uint64_t bits;
        addr_t flags = uart_lock(data);
        received = kfifo_out(&data->rx_fifo, buf, (unsigned int)len);
        uart_unlock(data, flags);
        if (received != 0) {
            return (ssize_t)received;
        }
        if (!wait || uart_in_interrupt() || flags != 0) {
            return -EAGAIN;
        }
        if (!kernel_running) {
            int rc = uart_stm32_poll_in(dev, buf);
            if (rc == 0) {
                return 1;
            }
            if (rc != -EAGAIN) {
                return rc;
            }
            continue;
        }
        int rc = nos_notify_wait_obj(&data->rx_ready, &bits,
                                     NOS_IPC_TIMEOUT_INFINITE);
        if (rc < 0) {
            return rc;
        }
    }
}

static void uart_stm32_panic_begin(const struct device *dev)
{
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;
    unsigned char ch;

    /* Panic 状态下避免常规自旋锁重入死锁 */
    data->panic = true;
    USART_ITConfig(config->regs, USART_IT_TXE, DISABLE);
#ifdef CONFIG_UART_DMA
    USART_DMACmd(config->regs, USART_DMAReq_Tx, DISABLE);
    if (data->dma_active) {
        unsigned int remaining;
        int rc = uart_stm32_dma_stop();
        if (rc >= 0) {
#ifdef CONFIG_SOC_STM32F4
            remaining = DMA_GetCurrDataCounter(uart_log_dev.dma_config->stream);
#else
            remaining = DMA_GetCurrDataCounter(uart_log_dev.dma_config->ch);
#endif
            if (remaining > data->dma_len) remaining = data->dma_len;
            data->dma_pos = data->dma_len - remaining;
            data->dma_active = false;
        }
    }
    while (data->dma_pos < data->dma_len) {
        int rc = uart_stm32_poll_out(dev, data->dma_buf[data->dma_pos++]);
        if (rc < 0) break;
    }
    data->dma_len = 0;
    data->dma_pos = 0;
#endif
    while (kfifo_out(&data->tx_fifo, &ch, 1) != 0) {
        int rc = uart_stm32_poll_out(dev, ch);
        if (rc < 0) {
            break;
        }
    }
}

static void uart_stm32_panic_write(const struct device *dev,
                                   const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (uart_stm32_poll_out(dev, buf[i]) < 0) {
            break;
        }
    }
}

static int uart_stm32_configure(const struct device *dev, const struct uart_config *cfg)
{
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;
    USART_InitTypeDef uart_init;

    if (cfg == NULL || cfg->baud_rate == 0) {
        return -EINVAL;
    }
    if (cfg->data_bits != UART_DATA_BITS_8 ||
        cfg->stop_bits != UART_STOP_BITS_1 ||
        cfg->flow_ctrl != UART_FLOW_NONE) {
        return -ENOTSUP;
    }

    uart_init.USART_BaudRate = cfg->baud_rate;
    uart_init.USART_WordLength = USART_WordLength_8b;
    uart_init.USART_StopBits = USART_StopBits_1;
    if (cfg->parity == UART_PARITY_ODD) {
        uart_init.USART_Parity = USART_Parity_Odd;
    } else if (cfg->parity == UART_PARITY_EVEN) {
        uart_init.USART_Parity = USART_Parity_Even;
    } else if (cfg->parity == UART_PARITY_NONE) {
        uart_init.USART_Parity = USART_Parity_No;
    } else {
        return -EINVAL;
    }
    uart_init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart_init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(config->regs, &uart_init);

    data->baud_rate = cfg->baud_rate;
    data->data_bits = cfg->data_bits;
    data->parity = cfg->parity;
    data->stop_bits = cfg->stop_bits;
    data->flow_ctrl = cfg->flow_ctrl;
    return 0;
}

static int uart_stm32_get_config(const struct device *dev, struct uart_config *cfg)
{
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;

    if (cfg == NULL) {
        return -EINVAL;
    }
    cfg->baud_rate = data->baud_rate != 0 ? data->baud_rate : config->baud_rate;
    cfg->data_bits = data->data_bits != 0 ? data->data_bits : UART_DATA_BITS_8;
    cfg->parity = data->parity;
    cfg->stop_bits = data->stop_bits != 0 ? data->stop_bits : UART_STOP_BITS_1;
    cfg->flow_ctrl = data->flow_ctrl;
    return 0;
}

static int uart_stm32_init(struct device *dev)
{
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;
    GPIO_InitTypeDef gpio_init;
    USART_InitTypeDef uart_init;
    NVIC_InitTypeDef irq_init;
    int rc;

#if defined(CONFIG_DEVICE_TREE) && defined(CONFIG_BOARD_QEMU_STM32)
    const struct nos_dt_node *chosen = nos_dt_find_path("/chosen");
    const struct nos_dt_node *node;
    uint32_t console;
    uint32_t address;
    uint32_t length;
    uint32_t irq;
    struct uart_stm32_config *dt_config = (struct uart_stm32_config *)dev->config;

    if (nos_dt_read_u32(chosen, "nos,console", 0, &console) != 0) {
        return -EINVAL;
    }
    node = nos_dt_find_phandle(console);
    if (!nos_dt_is_available(node) || !nos_dt_is_compatible(node, "st,stm32-uart") ||
        nos_dt_read_u32(node, "reg", 0, &address) != 0 ||
        nos_dt_read_u32(node, "reg", 1, &length) != 0 ||
        nos_dt_read_u32(node, "interrupts", 0, &irq) != 0 ||
        address != (uint32_t)(uintptr_t)USART1 || length < 0x400U ||
        irq != USART1_IRQn) {
        return -EINVAL;
    }
    dt_config->regs = USART1;
    dt_config->irq = irq;
#endif
    spin_lock_init(&data->lock);
    rc = kfifo_init(&data->rx_fifo, data->rx_buf, sizeof(data->rx_buf), 1);
    if (rc < 0) return rc;
    rc = kfifo_init(&data->tx_fifo, data->tx_buf, sizeof(data->tx_buf), 1);
    if (rc < 0) return rc;
    rc = nos_notification_init(&data->rx_ready);
    if (rc < 0) return rc;
    rc = nos_notification_init(&data->tx_ready);
    if (rc < 0) return rc;

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

    data->baud_rate = config->baud_rate;
    data->data_bits = UART_DATA_BITS_8;
    data->parity = UART_PARITY_NONE;
    data->stop_bits = UART_STOP_BITS_1;
    data->flow_ctrl = UART_FLOW_NONE;
#ifdef CONFIG_UART_DMA
    if (uart_log_dev.dma_config == NULL) return -ENODEV;
#ifdef CONFIG_SOC_STM32F4
    uart_log_dev.dma_config->init_type.DMA_Memory0BaseAddr =
        (uint32_t)(uintptr_t)data->dma_buf;
    uart_log_dev.dma_config->init_type.DMA_BufferSize = sizeof(data->dma_buf);
#else
    uart_log_dev.dma_config->init_type.DMA_MemoryBaseAddr =
        (uint32_t)(uintptr_t)data->dma_buf;
    uart_log_dev.dma_config->init_type.DMA_BufferSize = sizeof(data->dma_buf);
#endif
    rc = dma_config(uart_log_dev.dma_config);
    if (rc < 0) return rc;
#ifdef CONFIG_SOC_STM32F4
    DMA_ITConfig(uart_log_dev.dma_config->stream, DMA_IT_TE, ENABLE);
#else
    DMA_ITConfig(uart_log_dev.dma_config->ch, DMA_IT_TE, ENABLE);
#endif
    USART_DMACmd(config->regs, USART_DMAReq_Tx, ENABLE);
#endif
    USART_ITConfig(config->regs, USART_IT_TXE, DISABLE);
    USART_ITConfig(config->regs, USART_IT_RXNE, ENABLE);
    USART_Cmd(config->regs, ENABLE);

    irq_init.NVIC_IRQChannel = config->irq;
    irq_init.NVIC_IRQChannelPreemptionPriority = 3;
    irq_init.NVIC_IRQChannelSubPriority = 0;
    irq_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&irq_init);
    return 0;
}

static const struct uart_driver_api uart_stm32_api = {
    .poll_out = uart_stm32_poll_out,
    .poll_in = uart_stm32_poll_in,
    .write = uart_stm32_write,
    .read = uart_stm32_read,
    .configure = uart_stm32_configure,
    .get_config = uart_stm32_get_config,
    .panic_begin = uart_stm32_panic_begin,
    .panic_write = uart_stm32_panic_write,
};

static struct uart_stm32_config qemu_stm32_uart1_config = {
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
    .baud_rate = CONFIG_UART_BAUD_RATE,
    .irq = USART1_IRQn,
};

DEVICE_DEFINE(qemu_stm32_uart1, "UART1", uart_stm32_init, &qemu_stm32_uart1_config,
              &uart1_data, &uart_stm32_api, PRE_KERNEL_1);

void USART1_IRQHandler(void)
{
    const struct uart_stm32_config *config = &qemu_stm32_uart1_config;
    struct uart_stm32_data *data = &uart1_data;

    if (USART_GetITStatus(config->regs, USART_IT_RXNE) != RESET) {
        unsigned char ch = (unsigned char)USART_ReceiveData(config->regs);
        addr_t flags = uart_lock(data);
        unsigned int accepted = kfifo_in(&data->rx_fifo, &ch, 1);
        if (accepted == 0) data->rx_dropped++;
        uart_unlock(data, flags);
        if (accepted != 0 && kernel_running) {
            (void)nos_notify_signal_obj(&data->rx_ready, UART_NOTIFY_READY);
        }
    }
    if (USART_GetITStatus(config->regs, USART_IT_TXE) != RESET) {
        unsigned char ch;
        addr_t flags = uart_lock(data);
        unsigned int sent;
#ifdef CONFIG_UART_DMA
        if (data->dma_faulted && data->dma_pos < data->dma_len) {
            ch = data->dma_buf[data->dma_pos++];
            sent = 1;
        } else
#endif
        {
            sent = kfifo_out(&data->tx_fifo, &ch, 1);
        }
        if (sent != 0) {
            USART_SendData(config->regs, ch);
        }
        if (kfifo_used(&data->tx_fifo) == 0
#ifdef CONFIG_UART_DMA
            && data->dma_pos >= data->dma_len
#endif
        ) {
            USART_ITConfig(config->regs, USART_IT_TXE, DISABLE);
        }
        uart_unlock(data, flags);
        if (sent != 0 && kernel_running) {
            (void)nos_notify_signal_obj(&data->tx_ready, UART_NOTIFY_READY);
            kernel_log_flush();
        }
    }
}

#ifdef CONFIG_UART_DMA
#ifdef CONFIG_SOC_STM32F4
void DMA2_Stream7_IRQHandler(void)
#else
void DMA1_Channel4_IRQHandler(void)
#endif
{
    const struct device *dev = &__device_qemu_stm32_uart1;
    const struct uart_stm32_config *config = dev->config;
    struct uart_stm32_data *data = dev->data;
    bool failed;
    bool complete;
    unsigned int remaining;
    addr_t flags;

#ifdef CONFIG_SOC_STM32F4
    failed = DMA_GetITStatus(uart_log_dev.dma_config->stream, DMA_IT_TEIF7) != RESET;
    complete = DMA_GetITStatus(uart_log_dev.dma_config->stream, DMA_IT_TCIF7) != RESET;
    if (failed) DMA_ClearITPendingBit(uart_log_dev.dma_config->stream, DMA_IT_TEIF7);
    if (complete) DMA_ClearITPendingBit(uart_log_dev.dma_config->stream, DMA_IT_TCIF7);
#else
    failed = DMA_GetITStatus(DMA1_IT_TE4) != RESET;
    complete = DMA_GetITStatus(DMA1_IT_TC4) != RESET;
    if (failed) DMA_ClearITPendingBit(DMA1_IT_TE4);
    if (complete) DMA_ClearITPendingBit(DMA1_IT_TC4);
#endif
    if (!failed && !complete) return;
    flags = uart_lock(data);
    if (uart_stm32_dma_stop() < 0) {
        data->dma_errors++;
        data->dma_stalled = true;
        USART_DMACmd(config->regs, USART_DMAReq_Tx, DISABLE);
        uart_unlock(data, flags);
        return;
    }
#ifdef CONFIG_SOC_STM32F4
    remaining = DMA_GetCurrDataCounter(uart_log_dev.dma_config->stream);
#else
    remaining = DMA_GetCurrDataCounter(uart_log_dev.dma_config->ch);
#endif
    if (remaining > data->dma_len) remaining = data->dma_len;
    data->dma_active = false;
    if (failed) {
        data->dma_errors++;
        data->dma_faulted = true;
        data->dma_pos = data->dma_len - remaining;
        USART_DMACmd(config->regs, USART_DMAReq_Tx, DISABLE);
    } else {
        data->dma_len = 0;
        data->dma_pos = 0;
    }
    uart_stm32_kick_locked(dev);
    uart_unlock(data, flags);
    if (kernel_running) {
        (void)nos_notify_signal_obj(&data->tx_ready, UART_NOTIFY_READY);
        kernel_log_flush();
    }
}
#endif
