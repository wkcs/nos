/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 串口 1 (USART1 PA9/PA10) 驱动
 */

#ifndef BOOTLOADER_DRIVERS_UART_H_
#define BOOTLOADER_DRIVERS_UART_H_

#include <stdint.h>

/**
 * 初始化 USART1 (PA9 TX, PA10 RX, 115200 8N1)
 */
void uart1_init(void);

/**
 * 阻塞发送单个字符
 */
void uart_putc(char c);

/**
 * 阻塞发送以空字符结尾的字符串
 */
void uart_puts(const char *s);

/**
 * 发送原始数据缓冲区
 */
void uart_write(const uint8_t *data, uint16_t len);

#endif /* BOOTLOADER_DRIVERS_UART_H_ */

