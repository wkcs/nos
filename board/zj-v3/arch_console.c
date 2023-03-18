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

#include "board.h"

#ifdef CONFIG_UART_DMA
static char log_buf[UART_LOG_DMA_BUF_SIZE];
static bool dma_transport;
#endif

int consol_init(void)
{
#ifdef CONFIG_UART_DMA
    uart_log_dev.dma_config->init_type.DMA_MemoryBaseAddr = (uint32_t)log_buf;
#endif

    return uart_config(&uart_log_dev);
}

static char cmd_buf[256];
static uint8_t cmd_num;
void USART1_IRQHandler(void)
{
    char res;
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        res = USART_ReceiveData(USART1);
        if (res == '\r') {
            cmd_buf[cmd_num + 1] = 0;
            cmd_num = 0;
            usart_send("\r\n", 2);
        } else if (res != 0x09 && res != 0x1b) {
            if (res == '\b') {
                if (cmd_num > 0) {
                    cmd_num--;
                    cmd_buf[cmd_num] = 0;
                    usart_send(&res, 1);
                    usart_send(" ", 1);
                    usart_send(&res, 1);
                }
            } else {
                cmd_buf[cmd_num] = res;
                cmd_num++;
                usart_send(&res, 1);
            }
        }
        if (cmd_num == 255) {
            cmd_num = 0;
            usart_send("\r\n", 2);
        }
    }
}

int console_send_data(const char *buf, int len)
{
    return usart_send(buf, len);
}

void DMA1_Channel4_IRQHandler(void)
{
#ifdef CONFIG_UART_DMA
    unsigned int len;

    DMA_ClearITPendingBit(DMA1_IT_TC4);
    len = kernel_log_read(log_buf, UART_LOG_DMA_BUF_SIZE);
    if (len == 0) {
        dma_transport = false;
        return;
    }
    DMA_Cmd(uart_log_dev.dma_config->ch, DISABLE );
    DMA_SetCurrDataCounter(uart_log_dev.dma_config->ch, len);
    DMA_Cmd(uart_log_dev.dma_config->ch, ENABLE);
#endif
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
    DMA_Cmd(uart_log_dev.dma_config->ch, DISABLE );
    DMA_SetCurrDataCounter(uart_log_dev.dma_config->ch, len);
    DMA_Cmd(uart_log_dev.dma_config->ch, ENABLE);
#endif
}

static struct console_ops zj_console_ops = {
    .init = consol_init,
    .write = console_send_data,
    .send_log = arch_console_send_log,
};
console_register(tty0, &zj_console_ops);
