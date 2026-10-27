/**
 * Copyright (C) 2026 Nick Hu
 *
 * Contract tests for the optional raw TTY.
 */

#include <errno.h>
#include <stdio.h>
#if __has_include(<nos/tty.h>)
#include <nos/tty.h>
#define TEST_HAS_TTY 1
#endif

#ifdef TEST_HAS_TTY
static int reads;
static int backend_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    (void)context;
    (void)len;
    (void)wait;
    if (reads++ != 0) return -EAGAIN;
    buf[0] = 0;
    return 1;
}
static int backend_write(void *context, const unsigned char *buf, unsigned int len)
{
    (void)context;
    (void)buf;
    return len > 2 ? 2 : (int)len;
}
#endif

int main(void)
{
#ifndef TEST_HAS_TTY
    fputs("FAIL: raw TTY interface is unavailable\n", stderr);
    return 1;
#else
    struct nos_tty tty;
    const struct nos_tty_ops ops = { .read = backend_read, .write = backend_write };
    unsigned char data[4] = { 255 };

    if (nos_tty_init(&tty, &ops, NULL) != 0 ||
        nos_tty_read(&tty, data, sizeof(data), false) != 1 || data[0] != 0 ||
        nos_tty_read(&tty, data, sizeof(data), false) != -EAGAIN ||
        nos_tty_write(&tty, (const unsigned char *)"abc", 3) != 2 ||
        nos_tty_write(&tty, NULL, 1) != -EINVAL ||
        nos_tty_read(NULL, data, sizeof(data), false) != -EINVAL) {
        fputs("FAIL: raw TTY byte stream and error semantics\n", stderr);
        return 1;
    }
    puts("raw TTY checks passed");
    return 0;
#endif
}
