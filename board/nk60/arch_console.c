/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/printk.h>
#include <kernel/sem.h>
#include <lib/kfifo.h>

#include "board.h"

#ifdef CONFIG_UART_DMA
static char log_buf[UART_LOG_DMA_BUF_SIZE];
static bool dma_transport;
#endif

static struct kfifo g_console_fifo;
static char g_console_fifo_buf[CONFIG_CONSOLE_FIFO_BUF_SIZE];
sem_t g_rx_ready;

int consol_init(void)
{
    int rc;

    rc = kfifo_init(&g_console_fifo, g_console_fifo_buf, CONFIG_CONSOLE_FIFO_BUF_SIZE, 1);
    if (rc < 0) {
        pr_err("console fifo init failed\r\n");
        BUG_ON(1);
        return rc;
    }
    sem_init(&g_rx_ready, 0);

#ifdef CONFIG_UART_DMA
    uart_log_dev.dma_config->init_type.DMA_Memory0BaseAddr = (uint32_t)log_buf;
#endif

    return uart_config(&uart_log_dev);
}

void DMA2_Stream7_IRQHandler(void)
{
#ifdef CONFIG_UART_DMA
    size_t len;

    DMA_ClearITPendingBit(uart_log_dev.dma_config->stream, DMA_FLAG_TCIF7);
    len = kernel_log_read(log_buf, UART_LOG_DMA_BUF_SIZE);
    if (len == 0) {
        dma_transport = false;
        return;
    }
    DMA_Cmd(uart_log_dev.dma_config->stream, DISABLE);
    DMA_SetCurrDataCounter(uart_log_dev.dma_config->stream, len);
    DMA_Cmd(uart_log_dev.dma_config->stream, ENABLE);
#endif
}

void USART1_IRQHandler(void)
{
    char res;
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        res = USART_ReceiveData(USART1);
        kfifo_in(&g_console_fifo, &res, 1);
        sem_send_one(&g_rx_ready);
    }
}

int console_send_data(const char *buf, int len)
{
    return usart_send(buf, len);
}

static void arch_console_send_log(void)
{
#ifdef CONFIG_UART_DMA
    unsigned int len;

    if (dma_transport) {
        return;
    }

    len = kernel_log_read(log_buf, UART_LOG_DMA_BUF_SIZE);
    if (len == 0) {
        return;
    }
    dma_transport = true;
    DMA_Cmd(uart_log_dev.dma_config->stream, DISABLE);
    DMA_SetCurrDataCounter(uart_log_dev.dma_config->stream, len);
    DMA_Cmd(uart_log_dev.dma_config->stream, ENABLE);
#endif
}

static char arch_console_getc(void)
{
    char c = 0;

    sem_get(&g_rx_ready);
    kfifo_out(&g_console_fifo, &c, 1);
    return c;
}

static int arch_console_putc(char c)
{
    return usart_send(&c, 1);
}

static struct console_ops keyboard_v2_console_ops = {
    .init = consol_init,
    .write = console_send_data,
    .send_log = arch_console_send_log,
    .getc = arch_console_getc,
    .putc = arch_console_putc,
};
console_register(tty0, &keyboard_v2_console_ops);
