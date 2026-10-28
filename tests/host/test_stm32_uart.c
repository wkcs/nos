/**
 * Copyright (C) 2026 Nick Hu
 *
 * Exercise the STM32 UART driver against a simulated peripheral and real FIFO.
 */

#include <kernel/kernel.h>
#include <kernel/types.h>
#include <nos/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_UART_DMA
#include <board.h>
#endif
#include "../../drivers/serial/uart_stm32.c"

#ifdef CONFIG_DEVICE_TREE
static int invalid_dt_irq;
static const struct nos_dt_node mock_chosen;
static const struct nos_dt_node mock_uart;
const struct nos_dt_node *nos_dt_find_path(const char *path)
{
    return strcmp(path, "/chosen") == 0 ? &mock_chosen : NULL;
}
const struct nos_dt_node *nos_dt_find_phandle(uint32_t phandle)
{
    return phandle == 1 ? &mock_uart : NULL;
}
int nos_dt_is_available(const struct nos_dt_node *node)
{
    return node == &mock_uart;
}
int nos_dt_is_compatible(const struct nos_dt_node *node, const char *compatible)
{
    return node == &mock_uart && strcmp(compatible, "st,stm32-uart") == 0;
}
int nos_dt_read_u32(const struct nos_dt_node *node, const char *name,
                    size_t index, uint32_t *value)
{
    if (node == &mock_chosen && strcmp(name, "nos,console") == 0 && index == 0) {
        *value = 1;
    } else if (node == &mock_uart && strcmp(name, "reg") == 0 && index == 0) {
        *value = (uint32_t)(uintptr_t)USART1;
    } else if (node == &mock_uart && strcmp(name, "reg") == 0 && index == 1) {
        *value = 0x400;
    } else if (node == &mock_uart && strcmp(name, "interrupts") == 0 && index == 0) {
        *value = invalid_dt_irq ? 38 : USART1_IRQn;
    } else {
        return -EINVAL;
    }
    return 0;
}
#endif

USART_TypeDef simulated_usart;
GPIO_TypeDef simulated_gpio;
DMA_Stream_TypeDef simulated_dma;
addr_t kernel_running = 1;
int simulated_interrupt_context;
static int tx_interrupt_enabled;
static int rx_interrupt_enabled;
static int rx_ready;
static unsigned char rx_value;
static unsigned char wire[256];
static unsigned int wire_len;
static unsigned int notifications;
#ifdef CONFIG_UART_DMA
static int dma_enabled;
static uint16_t dma_remaining;
static uint32_t dma_pending;
static int dma_disable_delay;
static int dma_disable_pending;
static int dma_counter_read_while_stopping;
static struct dma_config_t test_dma = { .stream = DMA2_Stream7 };
struct uart_config_t uart_log_dev = { .dma_config = &test_dma };
int dma_config(struct dma_config_t *config) { (void)config; return 0; }
#endif

void kernel_log_flush(void) {}

addr_t disable_irq_save(void) { return 0; }
void enable_irq_save(addr_t flags) { (void)flags; }
void *kmalloc(size_t size, gfp_t flags) { (void)flags; return malloc(size); }
void kfree(void *ptr) { free(ptr); }
int nos_notification_init(struct nos_notification *n) { n->pending_bits = 0; return 0; }
int nos_notify_signal_obj(struct nos_notification *n, uint64_t bits)
{
    notifications++;
    n->pending_bits |= bits;
    return 0;
}
int nos_notify_wait_obj(struct nos_notification *n, uint64_t *bits, uint32_t timeout)
{
    (void)timeout;
    if (n->pending_bits == 0) return -EAGAIN;
    *bits = n->pending_bits;
    n->pending_bits = 0;
    return 0;
}
int USART_GetFlagStatus(USART_TypeDef *regs, uint32_t flag)
{
    (void)regs;
    return flag == USART_FLAG_RXNE ? rx_ready : SET;
}
int USART_GetITStatus(USART_TypeDef *regs, uint32_t irq)
{
    (void)regs;
    return irq == USART_IT_RXNE ? rx_interrupt_enabled && rx_ready : tx_interrupt_enabled;
}
void USART_SendData(USART_TypeDef *regs, uint16_t value)
{
    (void)regs;
    if (wire_len < sizeof(wire)) wire[wire_len++] = (unsigned char)value;
}
uint16_t USART_ReceiveData(USART_TypeDef *regs)
{
    (void)regs;
    rx_ready = 0;
    return rx_value;
}
void USART_ITConfig(USART_TypeDef *regs, uint32_t irq, int enabled)
{
    (void)regs;
    if (irq == USART_IT_RXNE) rx_interrupt_enabled = enabled;
    if (irq == USART_IT_TXE) tx_interrupt_enabled = enabled;
}
void USART_Init(USART_TypeDef *regs, USART_InitTypeDef *config) { (void)regs; (void)config; }
void USART_Cmd(USART_TypeDef *regs, int enabled) { (void)regs; (void)enabled; }
void RCC_AHB1PeriphClockCmd(uint32_t clock, int enabled) { (void)clock; (void)enabled; }
void RCC_APB2PeriphClockCmd(uint32_t clock, int enabled) { (void)clock; (void)enabled; }
void GPIO_PinAFConfig(GPIO_TypeDef *gpio, uint8_t pin, uint8_t af)
{ (void)gpio; (void)pin; (void)af; }
void GPIO_Init(GPIO_TypeDef *gpio, GPIO_InitTypeDef *config) { (void)gpio; (void)config; }
void NVIC_Init(NVIC_InitTypeDef *config) { (void)config; }
void USART_DMACmd(USART_TypeDef *regs, uint32_t request, int enabled)
{ (void)regs; (void)request; (void)enabled; }
void DMA_Cmd(DMA_Stream_TypeDef *stream, int enabled)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    if (!enabled && dma_enabled && dma_disable_delay > 0) {
        dma_disable_pending = 1;
    } else {
        dma_enabled = enabled;
    }
#else
    (void)enabled;
#endif
}
int DMA_GetCmdStatus(DMA_Stream_TypeDef *stream)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    if (dma_disable_pending && --dma_disable_delay <= 0) {
        dma_disable_pending = 0;
        dma_enabled = 0;
    }
    return dma_enabled;
#else
    return DISABLE;
#endif
}
void DMA_SetCurrDataCounter(DMA_Stream_TypeDef *stream, uint16_t count)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    dma_remaining = count;
#else
    (void)count;
#endif
}
uint16_t DMA_GetCurrDataCounter(DMA_Stream_TypeDef *stream)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    if (dma_disable_pending) dma_counter_read_while_stopping++;
    return dma_remaining;
#else
    return 0;
#endif
}
int DMA_GetITStatus(DMA_Stream_TypeDef *stream, uint32_t irq)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    return (dma_pending & irq) != 0;
#else
    (void)irq;
    return 0;
#endif
}
void DMA_ClearITPendingBit(DMA_Stream_TypeDef *stream, uint32_t irq)
{
    (void)stream;
#ifdef CONFIG_UART_DMA
    dma_pending &= ~irq;
#else
    (void)irq;
#endif
}
void DMA_ITConfig(DMA_Stream_TypeDef *stream, uint32_t irq, int enabled)
{ (void)stream; (void)irq; (void)enabled; }

#ifdef NOS_UART_BUFFERED_DRIVER
static void run_tx_interrupts(void)
{
    unsigned int guard = 32;
    while ((tx_interrupt_enabled
#ifdef CONFIG_UART_DMA
            || dma_enabled
#endif
           ) && guard-- > 0) {
        simulated_interrupt_context = 1;
#if defined(CONFIG_UART_DMA) && defined(NOS_UART_DMA_DRIVER)
        if (dma_enabled) {
            unsigned int offset = uart1_data.dma_len - dma_remaining;
            while (dma_remaining != 0) {
                USART_SendData(USART1, uart1_data.dma_buf[offset++]);
                dma_remaining--;
            }
            dma_pending |= DMA_IT_TCIF7;
            DMA2_Stream7_IRQHandler();
        } else
#endif
        USART1_IRQHandler();
        simulated_interrupt_context = 0;
    }
}

static void receive_byte(unsigned char value)
{
    rx_value = value;
    rx_ready = 1;
    simulated_interrupt_context = 1;
    USART1_IRQHandler();
    simulated_interrupt_context = 0;
}
#endif

int main(void)
{
#ifndef NOS_UART_BUFFERED_DRIVER
    fputs("FAIL: STM32 UART buffered IRQ transport is unavailable\n", stderr);
    return 1;
#elif defined(CONFIG_UART_DMA) && !defined(NOS_UART_DMA_DRIVER)
    fputs("FAIL: selected STM32 UART DMA transport is unavailable\n", stderr);
    return 1;
#else
    struct device *dev = &__device_qemu_stm32_uart1;
    unsigned char data[16];
    const unsigned char message[] = { 'A', 0, 0xff };

#ifdef CONFIG_DEVICE_TREE
    invalid_dt_irq = 1;
    if (uart_stm32_init(dev) != -EINVAL || rx_interrupt_enabled) {
        fputs("FAIL: reject unsupported device-tree IRQ before initializing hardware\n", stderr);
        return 1;
    }
    invalid_dt_irq = 0;
#endif
    if (uart_stm32_init(dev) != 0 || !rx_interrupt_enabled) {
        fputs("FAIL: initialize IRQ-backed UART\n", stderr);
        return 1;
    }
    if (uart_write_recordv(dev, message, 1, message + 1, 2) != 3 || wire_len != 0) {
        fputs("FAIL: enqueue complete binary record\n", stderr);
        return 1;
    }
    run_tx_interrupts();
    if (wire_len != sizeof(message) || memcmp(wire, message, sizeof(message)) != 0) {
        fputs("FAIL: IRQ drains a binary record in order\n", stderr);
        return 1;
    }
    if (uart_write_record(dev, (const unsigned char *)"12345678", 8) != 8 ||
#ifdef CONFIG_UART_DMA
        uart_write_record(dev, (const unsigned char *)"abcd", 4) != 4 ||
#endif
        uart_write_record(dev, (const unsigned char *)"X", 1) != -EAGAIN) {
        fputs("FAIL: full queue rejects an entire record\n", stderr);
        return 1;
    }
    run_tx_interrupts();
    receive_byte(0);
    receive_byte(255);
    if (uart_read(dev, data, sizeof(data), false) != 2 || data[0] != 0 || data[1] != 255 ||
        uart_read(dev, data, sizeof(data), false) != -EAGAIN) {
        fputs("FAIL: RX preserves bytes and reports empty queue\n", stderr);
        return 1;
    }
    notifications = 0;
    for (int i = 0; i < 10; i++) receive_byte((unsigned char)i);
    if (notifications != 8 || uart_read(dev, data, sizeof(data), false) != 8) {
        fputs("FAIL: RX overflow must not generate false notifications\n", stderr);
        return 1;
    }
#ifdef NOS_UART_STATS_API
    {
        struct uart_stats stats;
        if (uart_get_stats(dev, &stats) != 0 || stats.rx_dropped != 2 ||
            stats.tx_dropped_records != 1) {
            fputs("FAIL: expose UART RX and TX drops\n", stderr);
            return 1;
        }
    }
#else
    fputs("FAIL: UART overflow statistics are unavailable\n", stderr);
    return 1;
#endif
#ifdef CONFIG_UART_DMA
    wire_len = 0;
    if (uart_write_record(dev, (const unsigned char *)"PNC", 3) != 3 || !dma_enabled) {
        fputs("FAIL: start DMA before panic\n", stderr);
        return 1;
    }
    USART_SendData(USART1, uart1_data.dma_buf[0]);
    dma_remaining--;
    dma_disable_delay = 3;
    if (uart_panic_begin(dev) != 0 ||
        uart_panic_write(dev, (const unsigned char *)"!", 1) != 1 ||
        wire_len != 4 || memcmp(wire, "PNC!", 4) != 0) {
        fputs("FAIL: panic must drain only the unsent DMA bytes\n", stderr);
        return 1;
    }
    if (dma_counter_read_while_stopping != 0) {
        fputs("FAIL: DMA must stop before its remaining count is inspected\n", stderr);
        return 1;
    }
    memset(&uart1_data, 0, sizeof(uart1_data));
    wire_len = 0;
    tx_interrupt_enabled = 0;
    dma_enabled = 0;
    dma_pending = 0;
    if (uart_stm32_init(dev) != 0) {
        fputs("FAIL: reinitialize simulated UART\n", stderr);
        return 1;
    }
    if (uart_write_record(dev, (const unsigned char *)"ERR", 3) != 3 || !dma_enabled) {
        fputs("FAIL: start DMA for a queued record\n", stderr);
        return 1;
    }
    USART_SendData(USART1, uart1_data.dma_buf[0]);
    dma_remaining--;
    dma_pending |= DMA_IT_TEIF7;
    DMA2_Stream7_IRQHandler();
    run_tx_interrupts();
    if (wire_len != 3 || memcmp(wire, "ERR", 3) != 0) {
        fputs("FAIL: DMA failure must continue unsent bytes using TX IRQ\n", stderr);
        return 1;
    }
    {
        struct uart_stats stats;
        if (uart_get_stats(dev, &stats) != 0 || stats.dma_errors != 1) {
            fputs("FAIL: expose DMA transfer errors\n", stderr);
            return 1;
        }
    }
#endif
    wire_len = 0;
    if (uart_write_record(dev, (const unsigned char *)"abc", 3) != 3 ||
        uart_panic_begin(dev) != 0 ||
        uart_panic_write(dev, (const unsigned char *)"!", 1) != 1 ||
        wire_len != 4 || memcmp(wire, "abc!", 4) != 0) {
        fputs("FAIL: panic drains queued output before polling output\n", stderr);
        return 1;
    }
    puts("STM32 UART IRQ checks passed");
    return 0;
#endif
}
