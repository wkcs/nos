/**
 * Copyright (C) 2026 Nick Hu
 *
 * Canonical input, editing and echo behavior for the optional TTY mode.
 */

#include <nos/tty.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NOS_TTY_CANONICAL_API
struct fake_stream {
    const unsigned char *input;
    unsigned int input_len;
    unsigned int input_pos;
    unsigned char output[32];
    unsigned int output_len;
};

static int fake_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    struct fake_stream *stream = context;
    (void)len;
    (void)wait;
    if (stream->input_pos == stream->input_len) return -EAGAIN;
    buf[0] = stream->input[stream->input_pos++];
    return 1;
}

static int fake_write(void *context, const unsigned char *buf, unsigned int len)
{
    struct fake_stream *stream = context;
    memcpy(stream->output + stream->output_len, buf, len);
    stream->output_len += len;
    return (int)len;
}
#endif

int main(void)
{
#ifndef NOS_TTY_CANONICAL_API
    fputs("FAIL: configurable canonical TTY mode is unavailable\n", stderr);
    return 1;
#else
    const unsigned char input[] = { 'a', 'b', '\b', 'c', 'd', '\r' };
    const unsigned char expected_echo[] = { 'a', 'b', '\b', ' ', '\b', 'c', 'd', '\r', '\n' };
    struct fake_stream stream = { .input = input, .input_len = sizeof(input) };
    const struct nos_tty_ops ops = { .read = fake_read, .write = fake_write };
    struct nos_tty tty;
    unsigned char buf[8];

    if (nos_tty_init(&tty, &ops, &stream) != 0 ||
        nos_tty_set_mode(&tty, NOS_TTY_CANONICAL | NOS_TTY_ECHO |
                               NOS_TTY_ICRNL | NOS_TTY_ECHO_CRLF) != 0 ||
        nos_tty_read(&tty, buf, 2, false) != 2 || memcmp(buf, "ac", 2) != 0 ||
        nos_tty_read(&tty, buf, sizeof(buf), false) != 2 || memcmp(buf, "d\n", 2) != 0 ||
        stream.output_len != sizeof(expected_echo) ||
        memcmp(stream.output, expected_echo, sizeof(expected_echo)) != 0 ||
        nos_tty_read(&tty, buf, sizeof(buf), false) != -EAGAIN) {
        fputs("FAIL: canonical line editing, echo and partial reads\n", stderr);
        return 1;
    }
    {
        const unsigned char partial[] = { 'x', 'y', '\n' };
        struct fake_stream next = { .input = partial, .input_len = 2 };

        if (nos_tty_init(&tty, &ops, &next) != 0 ||
            nos_tty_set_mode(&tty, NOS_TTY_CANONICAL) != 0 ||
            nos_tty_read(&tty, buf, sizeof(buf), false) != -EAGAIN) {
            fputs("FAIL: incomplete line must remain buffered\n", stderr);
            return 1;
        }
        next.input_len = sizeof(partial);
        if (nos_tty_read(&tty, buf, sizeof(buf), false) != 3 ||
            memcmp(buf, "xy\n", 3) != 0) {
            fputs("FAIL: complete a previously buffered line\n", stderr);
            return 1;
        }
    }
    {
        const unsigned char long_line[] = "abcdefghijk\n";
        struct fake_stream next = { .input = long_line, .input_len = sizeof(long_line) - 1 };

        if (nos_tty_init(&tty, &ops, &next) != 0 ||
            nos_tty_set_mode(&tty, NOS_TTY_CANONICAL) != 0 ||
            nos_tty_read(&tty, buf, sizeof(buf), false) != 8 ||
            memcmp(buf, "abcdefg\n", 8) != 0) {
            fputs("FAIL: canonical input remains bounded and keeps the newline\n", stderr);
            return 1;
        }
    }
    puts("canonical TTY checks passed");
    return 0;
#endif
}
