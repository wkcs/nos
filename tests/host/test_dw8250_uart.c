/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Synopsys DW-8250 UART 驱动波特率分频、轮询收发、配置更新与 Panic 冲刷主机测试
 */

#include <assert.h>
#include <drivers/serial.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/serial/uart_dw8250.c"

int main(void)
{
    uint32_t regs[64];
    const struct uart_dw8250_config cfg = {
        .base         = (uintptr_t)regs,
        .clock_hz     = 24000000U,
        .baud_rate    = 1500000U,
        .poll_timeout = 8U,
        .irq_num      = 365U,
    };
    struct device dev = {
        .config = &cfg,
        .api    = &uart_dw8250_api,
        .ready  = true,
    };
    struct uart_config ucfg;
    unsigned char ch = 0xFFU;
    unsigned char out_msg[4] = { 'R', 'K', 'N', 'P' };

    memset(regs, 0, sizeof(regs));

    /* 1. 验证 RK3588 24MHz / 1.5Mbps 精确 DLAB=1 分频（divisor = 24e6 / (16 * 1.5e6) = 1） */
    assert(uart_dw8250_init(&dev) == 0);
    assert(regs[DW8250_DLL / 4U] == 1U && regs[DW8250_DLH / 4U] == 0U);
    assert((regs[DW8250_LCR / 4U] & DW8250_LCR_DLAB) == 0U);
    assert(regs[DW8250_FCR / 4U] == (DW8250_FCR_FIFO_EN | DW8250_FCR_RXSR | DW8250_FCR_TXSR));
    assert(regs[DW8250_MCR / 4U] == 0x03U);

    /* 2. 验证 THRE 就绪发送写 THR，未就绪超时返回 -ETIMEDOUT */
    assert(uart_poll_out(&dev, 'X') == -ETIMEDOUT);
    regs[DW8250_LSR / 4U] = DW8250_LSR_THRE;
    assert(uart_write(&dev, out_msg, sizeof(out_msg)) == (ssize_t)sizeof(out_msg));
    assert(regs[DW8250_THR / 4U] == 'P');

    /* 3. 验证非阻塞 RX 轮询与 NUL 字节保留 */
    assert(uart_poll_in(&dev, &ch) == -EAGAIN);
    regs[DW8250_LSR / 4U] = DW8250_LSR_THRE | DW8250_LSR_DR;
    regs[DW8250_RBR / 4U] = 0U;
    assert(uart_read(&dev, &ch, 1U, false) == 1 && ch == 0U);

    /* 4. 验证动态修改波特率 115200 (24e6 / (16*115200) = 13) */
    memset(&ucfg, 0, sizeof(ucfg));
    assert(uart_dw8250_get_config(&dev, &ucfg) == 0 && ucfg.baud_rate == 1500000U);
    ucfg.baud_rate = 115200U;
    assert(uart_configure(&dev, &ucfg) == 0);
    assert(regs[DW8250_DLL / 4U] == 13U);

    /* 5. 验证 Panic 屏蔽 IER 并直接输出 */
    regs[DW8250_IER / 4U] = DW8250_IER_ERBFI;
    uart_dw8250_panic_begin(&dev);
    assert(regs[DW8250_IER / 4U] == 0U);
    uart_dw8250_panic_write(&dev, (const unsigned char *)"!", 1U);
    assert(regs[DW8250_THR / 4U] == '!');

    printf("PASS: test_dw8250_uart (1.5M/115200 baud divisor, THRE/DR, NUL-safe RX, panic)\n");
    return 0;
}
