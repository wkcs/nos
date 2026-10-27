/**
 * Copyright (C) 2026 Nick Hu
 *
 * Verify that synchronous logging reports the output actually accepted.
 */

#include <kernel/printk.h>
#include <lib/kfifo.h>
#include <stdio.h>

static int sink_result;
static int record_calls;

int console_write(const char *buf, int len)
{
    (void)buf;
    (void)len;
    return sink_result;
}

int console_write_record(const char *buf, int len)
{
    record_calls++;
    return console_write(buf, len);
}

void console_send_log(void) {}

int kfifo_init(struct kfifo *fifo, void *buffer, unsigned int size, size_t esize)
{
    (void)size;
    (void)esize;
    fifo->data = buffer;
    return 0;
}

unsigned int kfifo_in(struct kfifo *fifo, const void *buf, unsigned int len)
{
    (void)fifo;
    (void)buf;
    return len;
}

unsigned int kfifo_out(struct kfifo *fifo, void *buf, unsigned int len)
{
    (void)fifo;
    (void)buf;
    (void)len;
    return 0;
}

int main(void)
{
    sink_result = 3;
    if (kernel_log_write("hello", 5) != 3) {
        fputs("FAIL: synchronous sink must report short writes\n", stderr);
        return 1;
    }
    if (record_calls != 1) {
        fputs("FAIL: log output must use record-aware console transport\n", stderr);
        return 1;
    }
    sink_result = -1;
    if (kernel_log_write("hello", 5) != 0) {
        fputs("FAIL: failed sink must not report accepted bytes\n", stderr);
        return 1;
    }
    puts("log sink checks passed");
    return 0;
}
