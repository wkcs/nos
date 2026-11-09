/**
 * Copyright (C) 2026 Nick Hu
 *
 * Canonical input, editing, termios flags, and echo behavior for TTY.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <nos/tty.h>

struct fake_stream {
    const unsigned char *input;
    unsigned int input_len;
    unsigned int input_pos;
    unsigned char output[64];
    unsigned int output_len;
};

static int fake_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    struct fake_stream *stream = context;
    (void)len;
    (void)wait;
    if (stream->input_pos >= stream->input_len) return -EAGAIN;
    buf[0] = stream->input[stream->input_pos++];
    return 1;
}

static int fake_write(void *context, const unsigned char *buf, unsigned int len)
{
    struct fake_stream *stream = context;
    if (stream->output_len + len > sizeof(stream->output)) {
        len = sizeof(stream->output) - stream->output_len;
    }
    memcpy(stream->output + stream->output_len, buf, len);
    stream->output_len += len;
    return (int)len;
}

static void test_canonical_line_editing_and_echo(void)
{
    const unsigned char input[] = { 'a', 'b', '\b', 'c', 'd', '\r' };
    const unsigned char expected_echo[] = { 'a', 'b', '\b', ' ', '\b', 'c', 'd', '\r', '\n' };
    struct fake_stream stream = { .input = input, .input_len = sizeof(input) };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    struct nos_termios tio = {
        .c_iflag = ICRNL,
        .c_oflag = ONLCR,
        .c_lflag = ICANON | ECHO | ECHOE,
    };
    unsigned char buf[8];

    assert(nos_tty_init(&tty, &ops, &stream) == 0);
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    /* 部分读取：读 2 字节应得到 "ac" */
    assert(nos_tty_read(&tty, buf, 2, false) == 2);
    assert(memcmp(buf, "ac", 2) == 0);

    /* 继续读取剩余：应得到 "d\n" */
    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == 2);
    assert(memcmp(buf, "d\n", 2) == 0);

    /* 验证回显内容包含退格擦除 \b \b 以及 \r -> \r\n */
    assert(stream.output_len == sizeof(expected_echo));
    assert(memcmp(stream.output, expected_echo, sizeof(expected_echo)) == 0);

    /* 缓冲已读完，后续读应返回 -EAGAIN */
    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == -EAGAIN);
}

static void test_canonical_del_and_erase(void)
{
    /* 0x7f (DEL) 同样应触发擦除回显序列 \b \b */
    const unsigned char input[] = { 'x', 0x7f, 'y', '\n' };
    const unsigned char expected_echo[] = { 'x', '\b', ' ', '\b', 'y', '\n' };
    struct fake_stream stream = { .input = input, .input_len = sizeof(input) };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    struct nos_termios tio = {
        .c_lflag = ICANON | ECHO | ECHOE,
    };
    unsigned char buf[8];

    assert(nos_tty_init(&tty, &ops, &stream) == 0);
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == 2);
    assert(memcmp(buf, "y\n", 2) == 0);

    assert(stream.output_len == sizeof(expected_echo));
    assert(memcmp(stream.output, expected_echo, sizeof(expected_echo)) == 0);
}

static void test_igncr_handling(void)
{
    /* 当设置 IGNCR 时，\r 直接被丢弃，不转换为 \n */
    const unsigned char input[] = { 'h', '\r', 'i', '\n' };
    struct fake_stream stream = { .input = input, .input_len = sizeof(input) };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    struct nos_termios tio = {
        .c_iflag = IGNCR | ICRNL,
        .c_lflag = ICANON,
    };
    unsigned char buf[8];

    assert(nos_tty_init(&tty, &ops, &stream) == 0);
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == 3);
    assert(memcmp(buf, "hi\n", 3) == 0);
}

static void test_buffered_line_continuation(void)
{
    const unsigned char partial[] = { 'x', 'y', '\n' };
    struct fake_stream next = { .input = partial, .input_len = 2 };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    struct nos_termios tio = { .c_lflag = ICANON };
    unsigned char buf[8];

    assert(nos_tty_init(&tty, &ops, &next) == 0);
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    /* 行未完成，应保持缓冲并返回 -EAGAIN */
    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == -EAGAIN);

    /* 追加换行符后读取成功 */
    next.input_len = sizeof(partial);
    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == 3);
    assert(memcmp(buf, "xy\n", 3) == 0);
}

static void test_bounded_line_buffer(void)
{
    const unsigned char long_line[] = "abcdefghijk\n";
    struct fake_stream next = { .input = long_line, .input_len = sizeof(long_line) - 1 };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    struct nos_termios tio = { .c_lflag = ICANON };
    unsigned char buf[16];

    assert(nos_tty_init(&tty, &ops, &next) == 0);
    assert(nos_tty_ioctl(&tty, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);

    /* 当行超出限制时（测试宏设定为 8），截断超长部分并保留最终换行符 */
    assert(nos_tty_read(&tty, buf, sizeof(buf), false) == 8);
    assert(memcmp(buf, "abcdefg\n", 8) == 0);
}

int main(void)
{
    test_canonical_line_editing_and_echo();
    test_canonical_del_and_erase();
    test_igncr_handling();
    test_buffered_line_continuation();
    test_bounded_line_buffer();

    puts("canonical TTY checks passed");
    return 0;
}
