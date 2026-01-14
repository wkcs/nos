/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/sem.h>
#include <lib/kfifo.h>

#include "board.h"

#ifdef CONFIG_UART_DMA
static char log_buf[UART_LOG_DMA_BUF_SIZE];
static bool dma_transport;
#endif

#define CONFIG_CONSOLE_FIFO_BUF_SIZE 256
static volatile char g_console_buf[CONFIG_CONSOLE_FIFO_BUF_SIZE];
static volatile int g_console_head = 0;
static volatile int g_console_tail = 0;

int consol_init(void) {
#ifdef CONFIG_UART_DMA
  uart_log_dev.dma_config->init_type.DMA_Memory0BaseAddr = (uint32_t)log_buf;
#endif

  return uart_config(&uart_log_dev);
}

void USART1_IRQHandler(void) {
  char res;
  if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
    res = USART_ReceiveData(USART1);
    
    int next = (g_console_head + 1) % CONFIG_CONSOLE_FIFO_BUF_SIZE;
    if (next != g_console_tail) {
        g_console_buf[g_console_head] = res;
        g_console_head = next;
    }
    // Echo removed: Shell handles echo
  }
}

int console_send_data(const char *buf, int len) { return usart_send(buf, len); }

int arch_console_putc(char c) { return usart_send(&c, 1); }

char arch_console_getc(void) {
  if (g_console_head == g_console_tail) {
      return 0;
  }
  char c = g_console_buf[g_console_tail];
  g_console_tail = (g_console_tail + 1) % CONFIG_CONSOLE_FIFO_BUF_SIZE;
  return c;
}

void DMA1_Channel4_IRQHandler(void) {
#ifdef CONFIG_UART_DMA
  unsigned int len;

  DMA_ClearITPendingBit(DMA1_IT_TC4);
  len = kernel_log_read(log_buf, UART_LOG_DMA_BUF_SIZE);
  if (len == 0) {
    dma_transport = false;
    return;
  }
  DMA_Cmd(uart_log_dev.dma_config->ch, DISABLE);
  DMA_SetCurrDataCounter(uart_log_dev.dma_config->ch, len);
  DMA_Cmd(uart_log_dev.dma_config->ch, ENABLE);
#endif
}

static void arch_console_send_log(void) {
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
  DMA_Cmd(uart_log_dev.dma_config->ch, DISABLE);
  DMA_SetCurrDataCounter(uart_log_dev.dma_config->ch, len);
  DMA_Cmd(uart_log_dev.dma_config->ch, ENABLE);
#endif
}

static struct console_ops zj_console_ops = {
    .init = consol_init,
    .write = console_send_data,
    .getc = arch_console_getc,
    .putc = arch_console_putc,
    .send_log = arch_console_send_log,
};
console_register(tty0, &zj_console_ops);
