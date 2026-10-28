/**
 * Copyright (C) 2026 Nick Hu
 *
 * Verify record buffering, backpressure and panic draining.
 */

#include <kernel/kernel.h>
#include <kernel/types.h>
#include <kernel/printk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

addr_t kernel_running = 1;
static unsigned int room;
static int panic_mode;
static int console_online = 1;
static char wire[128];
static unsigned int wire_len;
static int split_record_seen;

addr_t disable_irq_save(void) { return 0; }
void enable_irq_save(addr_t flags) { (void)flags; }
void *kmalloc(size_t size, gfp_t flags) { (void)flags; return malloc(size); }
void kfree(void *ptr) { free(ptr); }
int console_write_room(void) { return console_online ? (int)room : -ENODEV; }
bool console_is_panic(void) { return panic_mode != 0; }
int console_write(const char *buf, int len)
{
    memcpy(wire + wire_len, buf, (size_t)len);
    wire_len += (unsigned int)len;
    return len;
}
int console_write_record(const char *buf, int len)
{
    if (!console_online) return -ENODEV;
    if ((unsigned int)len > room) return -EAGAIN;
    room -= (unsigned int)len;
    return console_write(buf, len);
}
#ifdef NOS_LOG_VECTOR_API
int console_write_recordv(const char *first, unsigned int first_len,
                          const char *second, unsigned int second_len)
{
    unsigned int total = first_len + second_len;
    if (total > room) return -EAGAIN;
    if (second_len != 0) split_record_seen = 1;
    room -= total;
    if (first_len != 0) console_write(first, (int)first_len);
    if (second_len != 0) console_write(second, (int)second_len);
    return (int)total;
}
#endif
void console_send_log(void) {}

#ifdef NOS_LOG_BACKEND_API
static unsigned int backend_a_count, backend_b_count;
static int capture_log(const struct kernel_log_record *record, const void *data,
                        void *priv)
{
    unsigned int *count = priv;
    if (record->length == 2 && memcmp(data, "ok", 2) == 0) {
        (*count)++;
        return (int)record->length;
    }
    return 0;
}
#endif

int main(void)
{
#ifndef NOS_LOG_BACKEND_API
    fputs("FAIL: independent log backend API is unavailable\n", stderr);
    return 1;
#else
    struct kernel_log_backend a = { .write = capture_log, .priv = &backend_a_count };
    struct kernel_log_backend b = { .write = capture_log, .priv = &backend_b_count };
    kernel_log_init();
    room = 0;
    if (kernel_log_backend_register(&a) != 0 || kernel_log_backend_register(&b) != 0 ||
        kernel_log_write("ok", 2) != 2 || backend_a_count != 1 || backend_b_count != 1) {
        fputs("FAIL: blocked console must not block registered backends\n", stderr);
        return 1;
    }
#ifdef NOS_LOG_CONSOLE_BACKEND_API
    kernel_log_backend_unregister(kernel_log_console_backend());
    room = 20;
    kernel_log_flush();
    if (wire_len != 0) {
        fputs("FAIL: unregistered console must not drain pending records\n", stderr);
        return 1;
    }
    if (kernel_log_write("ok", 2) != 2 || backend_a_count != 2 || backend_b_count != 2 || wire_len != 0) {
        fputs("FAIL: console must be an independently removable backend\n", stderr);
        return 1;
    }
    if (kernel_log_backend_register(kernel_log_console_backend()) != 0) return 1;
    kernel_log_flush();
    if (wire_len != 2 || memcmp(wire, "ok", 2) != 0) return 1;
    wire_len = 0;
#else
    fputs("FAIL: console is not a registered log backend\n", stderr);
    return 1;
#endif
    kernel_log_backend_unregister(&a);
    kernel_log_backend_unregister(&b);
#endif
#ifndef NOS_LOG_RECORD_RING
    fputs("FAIL: deferred record log ring is unavailable\n", stderr);
    return 1;
#elif !defined(NOS_LOG_METADATA_API)
    fputs("FAIL: log record severity and timestamp metadata are unavailable\n", stderr);
    return 1;
#elif !defined(NOS_LOG_VECTOR_API)
    fputs("FAIL: log ring lacks bounded-stack split-record transport\n", stderr);
    return 1;
#else
    struct kernel_log_stats stats;

    kernel_running = 0;
    console_online = 0;
    kernel_log_init();
    if (kernel_log_write("early", 5) != 5 || wire_len != 0) {
        fputs("FAIL: retain early log before console initialization\n", stderr);
        return 1;
    }
    console_online = 1;
    kernel_running = 1;
    room = 5;
    kernel_log_flush();
    if (wire_len != 5 || memcmp(wire, "early", 5) != 0) {
        fputs("FAIL: replay early log after console initialization\n", stderr);
        return 1;
    }
    wire_len = 0;
    room = 0;
    kernel_log_init();
    {
        char discarded[3];
        if (kernel_log_write("one", 3) != 3 || kernel_log_read(discarded, 3) != 3 ||
            kernel_log_write("two", 3) != 3 || kernel_log_read(discarded, 3) != 3 ||
            kernel_log_write("abcdef", 6) != 6) {
            fputs("FAIL: set up wrapped log record\n", stderr);
            return 1;
        }
        room = 6;
        kernel_log_flush();
        if (!split_record_seen || wire_len != 6 || memcmp(wire, "abcdef", 6) != 0) {
            fputs("FAIL: wrapped log record must be transmitted atomically\n", stderr);
            return 1;
        }
        wire_len = 0;
        room = 0;
    }
    {
        char read_buf[3];
        struct kernel_log_record record;
        if (kernel_log_write("X", 1) != 1 ||
            kernel_log_write_record("YZ", 2, LOG_WARNING, 3, 1234) != 2 ||
            kernel_log_read(read_buf, 1) != 1 || read_buf[0] != 'X' ||
            kernel_log_read(read_buf, 1) != 0 ||
            kernel_log_read_record(read_buf, 2, &record) != 2 ||
            record.level != LOG_WARNING || record.flags != 3 || record.timestamp_ms != 1234 ||
            record.length != 2 || memcmp(read_buf, "YZ", 2) != 0) {
            fputs("FAIL: log reader returns complete queued records\n", stderr);
            return 1;
        }
    }
    if (kernel_log_write("A", 1) != 1 || kernel_log_write("BC", 2) != 2 ||
        wire_len != 0) {
        fputs("FAIL: retain complete records while UART is full\n", stderr);
        return 1;
    }
    room = 3;
    kernel_log_flush();
    if (wire_len != 3 || memcmp(wire, "ABC", 3) != 0) {
        fputs("FAIL: drain complete records in order\n", stderr);
        return 1;
    }
    room = 0;
    if (kernel_log_write("123456", 6) != 6 || kernel_log_write("abcdef", 6) != 6 ||
        kernel_log_write("X", 1) != 0) {
        fputs("FAIL: reject an entire record when log ring is full\n", stderr);
        return 1;
    }
    kernel_log_get_stats(&stats);
    if (stats.dropped_records != 1 || stats.high_water != 28) {
        fputs("FAIL: report overflow and high-water mark\n", stderr);
        return 1;
    }
    panic_mode = 1;
    kernel_log_panic_flush();
    if (wire_len != 15 || memcmp(wire + 3, "123456abcdef", 12) != 0) {
        fputs("FAIL: panic drains retained records without UART IRQs\n", stderr);
        return 1;
    }
    puts("log record ring checks passed");
    return 0;
#endif
}
