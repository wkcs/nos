/**
 * Copyright (C) 2026 Nick Hu
 *
 * Contract tests for the standardized TTY subsystem, termios ioctl, and nos_tty_fops.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fs/vfs.h>
#if __has_include(<nos/tty.h>)
#include <nos/tty.h>
#define TEST_HAS_TTY 1
#endif

#ifdef TEST_HAS_TTY

static unsigned char g_backend_rx_buf[64];
static unsigned int g_backend_rx_len;
static unsigned int g_backend_rx_pos;

static unsigned char g_backend_tx_buf[64];
static unsigned int g_backend_tx_len;

static int mock_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    (void)context;
    (void)wait;
    if (g_backend_rx_pos >= g_backend_rx_len) {
        return -EAGAIN;
    }
    unsigned int to_copy = len;
    if (g_backend_rx_pos + to_copy > g_backend_rx_len) {
        to_copy = g_backend_rx_len - g_backend_rx_pos;
    }
    memcpy(buf, &g_backend_rx_buf[g_backend_rx_pos], to_copy);
    g_backend_rx_pos += to_copy;
    return (int)to_copy;
}

static int mock_write(void *context, const unsigned char *buf, unsigned int len)
{
    (void)context;
    if (g_backend_tx_len + len > sizeof(g_backend_tx_buf)) {
        len = sizeof(g_backend_tx_buf) - g_backend_tx_len;
    }
    memcpy(&g_backend_tx_buf[g_backend_tx_len], buf, len);
    g_backend_tx_len += len;
    return (int)len;
}

static void test_raw_tty_basics(void)
{
    struct nos_tty tty;
    const struct nos_tty_ops ops = { .read = mock_read, .write = mock_write };
    unsigned char data[4] = { 0 };

    g_backend_rx_buf[0] = 42;
    g_backend_rx_len = 1;
    g_backend_rx_pos = 0;
    g_backend_tx_len = 0;

    assert(nos_tty_init(&tty, &ops, NULL) == 0);

    /* 验证读取 1 字节 */
    assert(nos_tty_read(&tty, data, sizeof(data), false) == 1);
    assert(data[0] == 42);

    /* 无数据应返回 -EAGAIN */
    assert(nos_tty_read(&tty, data, sizeof(data), false) == -EAGAIN);

    /* 验证写入 */
    assert(nos_tty_write(&tty, (const unsigned char *)"abc", 3) == 3);
    assert(g_backend_tx_len == 3);
    assert(memcmp(g_backend_tx_buf, "abc", 3) == 0);

    /* 边界检查 */
    assert(nos_tty_write(&tty, NULL, 1) == -EINVAL);
    assert(nos_tty_read(NULL, data, sizeof(data), false) == -EINVAL);
}

static void test_termios_ioctl(void)
{
    struct nos_tty tty;
    const struct nos_tty_ops ops = { .read = mock_read, .write = mock_write };
    struct nos_termios tio;

    assert(nos_tty_init(&tty, &ops, NULL) == 0);

    /* 初始模式全为 0 */
    assert(nos_tty_ioctl(&tty, TCGETS, (unsigned long)(uintptr_t)&tio) == 0);
    assert(tio.c_iflag == 0);
    assert(tio.c_oflag == 0);
    assert(tio.c_lflag == 0);

    /* 配置 termios */
    tio.c_iflag = ICRNL | IGNCR;
    tio.c_oflag = ONLCR;
    tio.c_lflag = ICANON | ECHO | ECHOE;
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    /* 重新获取并验证 */
    memset(&tio, 0, sizeof(tio));
    assert(nos_tty_ioctl(&tty, TCGETS, (unsigned long)(uintptr_t)&tio) == 0);
    assert(tio.c_iflag == (ICRNL | IGNCR));
    assert(tio.c_oflag == ONLCR);
    assert(tio.c_lflag == (ICANON | ECHO | ECHOE));

    /* 无效参数与未知命令 */
    assert(nos_tty_ioctl(&tty, TCGETS, 0) == -EINVAL);
    assert(nos_tty_ioctl(&tty, TCSETS, 0) == -EINVAL);
    assert(nos_tty_ioctl(&tty, 0x9999, 0) == -ENOSYS);
}

static void test_onlcr_output(void)
{
    struct nos_tty tty;
    const struct nos_tty_ops ops = { .read = mock_read, .write = mock_write };
    struct nos_termios tio = { 0 };

    assert(nos_tty_init(&tty, &ops, NULL) == 0);
    g_backend_tx_len = 0;

    /* 未开启 ONLCR 时，\n 原样输出 */
    assert(nos_tty_write(&tty, (const unsigned char *)"a\nb", 3) == 3);
    assert(g_backend_tx_len == 3);
    assert(memcmp(g_backend_tx_buf, "a\nb", 3) == 0);

    /* 开启 ONLCR */
    tio.c_oflag = ONLCR;
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    g_backend_tx_len = 0;
    assert(nos_tty_write(&tty, (const unsigned char *)"x\ny", 3) == 3);
    assert(g_backend_tx_len == 4);
    assert(memcmp(g_backend_tx_buf, "x\r\ny", 4) == 0);
}

static void test_nos_tty_fops(void)
{
    struct nos_tty tty;
    const struct nos_tty_ops ops = { .read = mock_read, .write = mock_write };
    struct nos_termios tio;
    struct inode inode = { .i_private = &tty };
    struct file file = { .f_flags = 0, .f_op = &nos_tty_fops };
    loff_t pos = 0;
    char buf[16] = { 0 };

    assert(nos_tty_init(&tty, &ops, NULL) == 0);

    /* fops open */
    assert(nos_tty_fops.open(&inode, &file) == 0);
    assert(file.private_data == &tty);

    /* fops ioctl */
    assert(nos_tty_fops.ioctl(&file, TCGETS, (unsigned long)(uintptr_t)&tio) == 0);
    tio.c_oflag = ONLCR;
    assert(nos_tty_fops.ioctl(&file, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    /* fops write (ONLCR 展开) */
    g_backend_tx_len = 0;
    assert(nos_tty_fops.write(&file, "ok\n", 3, &pos) == 3);
    assert(g_backend_tx_len == 4);
    assert(memcmp(g_backend_tx_buf, "ok\r\n", 4) == 0);

    /* fops read */
    g_backend_rx_buf[0] = 'Z';
    g_backend_rx_len = 1;
    g_backend_rx_pos = 0;
    assert(nos_tty_fops.read(&file, buf, sizeof(buf), &pos) == 1);
    assert(buf[0] == 'Z');

    /* fops release */
    assert(nos_tty_fops.release(&inode, &file) == 0);
}

#endif

int main(void)
{
#ifndef TEST_HAS_TTY
    fputs("FAIL: raw TTY interface is unavailable\n", stderr);
    return 1;
#else
    test_raw_tty_basics();
    test_termios_ioctl();
    test_onlcr_output();
    test_nos_tty_fops();
    puts("raw TTY checks passed");
    return 0;
#endif
}
