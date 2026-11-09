/**
 * Copyright (C) 2026 Nick Hu
 *
 * Serial driver API contract test.
 */

#include <drivers/serial.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

static unsigned char mock_tx_buf[128];
static size_t mock_tx_len = 0;
static unsigned char mock_rx_buf[128];
static size_t mock_rx_len = 0;
static size_t mock_rx_pos = 0;
static struct uart_config mock_saved_cfg;
static bool mock_panic_begun = false;

static ssize_t mock_uart_write(const struct device *dev, const unsigned char *buf, size_t len)
{
    (void)dev;
    if (len > sizeof(mock_tx_buf) - mock_tx_len) {
        len = sizeof(mock_tx_buf) - mock_tx_len;
    }
    memcpy(mock_tx_buf + mock_tx_len, buf, len);
    mock_tx_len += len;
    return (ssize_t)len;
}

static ssize_t mock_uart_read(const struct device *dev, unsigned char *buf, size_t len, bool wait)
{
    (void)dev;
    (void)wait;
    size_t avail = mock_rx_len - mock_rx_pos;
    if (avail == 0) {
        return -EAGAIN;
    }
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, mock_rx_buf + mock_rx_pos, len);
    mock_rx_pos += len;
    return (ssize_t)len;
}

static int mock_uart_poll_out(const struct device *dev, unsigned char out)
{
    (void)dev;
    if (mock_tx_len < sizeof(mock_tx_buf)) {
        mock_tx_buf[mock_tx_len++] = out;
        return 0;
    }
    return -EAGAIN;
}

static int mock_uart_poll_in(const struct device *dev, unsigned char *in)
{
    (void)dev;
    if (mock_rx_pos < mock_rx_len) {
        *in = mock_rx_buf[mock_rx_pos++];
        return 0;
    }
    return -EAGAIN;
}

static int mock_uart_configure(const struct device *dev, const struct uart_config *cfg)
{
    (void)dev;
    mock_saved_cfg = *cfg;
    return 0;
}

static int mock_uart_get_config(const struct device *dev, struct uart_config *cfg)
{
    (void)dev;
    *cfg = mock_saved_cfg;
    return 0;
}

static void mock_uart_panic_begin(const struct device *dev)
{
    (void)dev;
    mock_panic_begun = true;
}

static void mock_uart_panic_write(const struct device *dev, const unsigned char *buf, size_t len)
{
    (void)dev;
    if (len > sizeof(mock_tx_buf) - mock_tx_len) {
        len = sizeof(mock_tx_buf) - mock_tx_len;
    }
    memcpy(mock_tx_buf + mock_tx_len, buf, len);
    mock_tx_len += len;
}

static const struct uart_driver_api mock_api = {
    .poll_in = mock_uart_poll_in,
    .poll_out = mock_uart_poll_out,
    .read = mock_uart_read,
    .write = mock_uart_write,
    .configure = mock_uart_configure,
    .get_config = mock_uart_get_config,
    .panic_begin = mock_uart_panic_begin,
    .panic_write = mock_uart_panic_write,
};

static const struct device mock_dev = {
    .api = &mock_api,
    .ready = true,
};

static const struct device not_ready_dev = {
    .api = &mock_api,
    .ready = false,
};

int main(void)
{
    /* 1. 测试未就绪设备与参数校验 */
    assert(uart_poll_out(&not_ready_dev, 'a') == -ENODEV);
    assert(uart_poll_in(&not_ready_dev, mock_rx_buf) == -ENODEV);
    assert(uart_poll_in(&mock_dev, NULL) == -EINVAL);
    assert(uart_write(&not_ready_dev, (const unsigned char *)"a", 1) == -ENODEV);
    assert(uart_write(&mock_dev, NULL, 1) == -EINVAL);
    assert(uart_read(&not_ready_dev, mock_rx_buf, 1, false) == -ENODEV);
    assert(uart_read(&mock_dev, NULL, 1, false) == -EINVAL);
    assert(uart_configure(&not_ready_dev, &mock_saved_cfg) == -ENODEV);
    assert(uart_configure(&mock_dev, NULL) == -EINVAL);
    assert(uart_get_config(&not_ready_dev, &mock_saved_cfg) == -ENODEV);
    assert(uart_get_config(&mock_dev, NULL) == -EINVAL);

    /* 2. 测试流式写 (uart_write) */
    unsigned char test_data[] = "hello uart";
    ssize_t rc = uart_write(&mock_dev, test_data, sizeof(test_data) - 1);
    assert(rc == (ssize_t)(sizeof(test_data) - 1));
    assert(memcmp(mock_tx_buf, "hello uart", 10) == 0);

    /* 3. 测试轮询输出 (uart_poll_out) */
    assert(uart_poll_out(&mock_dev, '!') == 0);
    assert(mock_tx_len == 11);
    assert(mock_tx_buf[10] == '!');

    /* 4. 测试轮询输入与流式读 (uart_poll_in / uart_read) */
    mock_rx_buf[0] = 'X';
    mock_rx_buf[1] = 'Y';
    mock_rx_buf[2] = 'Z';
    mock_rx_len = 3;
    mock_rx_pos = 0;
    unsigned char in_char = 0;
    assert(uart_poll_in(&mock_dev, &in_char) == 0);
    assert(in_char == 'X');

    unsigned char read_buf[4];
    ssize_t read_rc = uart_read(&mock_dev, read_buf, sizeof(read_buf), false);
    assert(read_rc == 2);
    assert(read_buf[0] == 'Y' && read_buf[1] == 'Z');
    assert(uart_read(&mock_dev, read_buf, sizeof(read_buf), false) == -EAGAIN);

    /* 5. 测试硬件参数配置 (uart_configure / uart_get_config) */
    struct uart_config cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_BITS_8,
        .parity = UART_PARITY_NONE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_FLOW_NONE,
    };
    assert(uart_configure(&mock_dev, &cfg) == 0);
    struct uart_config read_cfg;
    assert(uart_get_config(&mock_dev, &read_cfg) == 0);
    assert(read_cfg.baud_rate == 115200);
    assert(read_cfg.data_bits == UART_DATA_BITS_8);

    /* 6. 测试 Panic 应急写 (uart_panic_begin / uart_panic_write) */
    uart_panic_begin(&mock_dev);
    assert(mock_panic_begun == true);
    uart_panic_write(&mock_dev, (const unsigned char *)"PANIC", 5);
    assert(memcmp(mock_tx_buf + 11, "PANIC", 5) == 0);

    puts("serial API checks passed");
    return 0;
}
