/**
 * Copyright (C) 2026 Nick Hu
 *
 * Regression tests for log severity, truncation and nested output.
 */

#include <kernel/printk.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char captured[512];
static unsigned int captured_len;
static int writes;
static int reenter;
static int failures;
static int clock_calls;
static enum log_level captured_level;
static uint8_t captured_flags;
static uint32_t captured_timestamp;

void kernel_log_record_truncated(void) {}

#define CHECK(condition, description)                                           \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL: %s\n", description);                        \
            failures++;                                                         \
        }                                                                       \
    } while (0)

uint64_t cpu_run_time_us(void)
{
    clock_calls++;
    return 1234567;
}

unsigned int kernel_log_write(const void *buf, unsigned int len)
{
    writes++;
    if (reenter) {
        reenter = 0;
        pr_log(true, LOG_ERROR, "INNER");
    }
    captured_len = len < sizeof(captured) ? len : sizeof(captured);
    memcpy(captured, buf, captured_len);
    return len;
}

unsigned int kernel_log_write_record(const void *buf, unsigned int len,
                                     enum log_level level, uint8_t flags,
                                     uint32_t timestamp_ms)
{
    captured_level = level;
    captured_flags = flags;
    captured_timestamp = timestamp_ms;
    return kernel_log_write(buf, len);
}

int main(void)
{
    char long_message[300];

    pr_log(false, LOG_INFO, "early");
    CHECK(clock_calls == 0, "early logging does not read an uninitialized clock");
#ifdef NOS_LOG_CLOCK_READY_API
    kernel_log_clock_ready();
    pr_log(true, LOG_INFO, "metadata");
    CHECK(captured_level == LOG_INFO && captured_flags == LOG_RECORD_UNTAGGED &&
          captured_timestamp == 1234, "record severity and timestamp metadata");
#else
    fputs("FAIL: explicit logging clock initialization is unavailable\n", stderr);
    failures++;
#endif

    set_log_level(LOG_INFO);
    writes = 0;
    pr_log(true, LOG_DEBUG, "debug");
    pr_log(true, LOG_INFO, "info");
    CHECK(writes == 1, "minimum severity includes INFO and excludes DEBUG");

    set_log_level(LOG_FATAL);
    writes = 0;
    pr_log(true, LOG_FATAL, "fatal");
    CHECK(writes == 1, "fatal threshold includes fatal output");

    set_log_level(LOG_DISABLE);
    writes = 0;
    pr_log(true, LOG_FATAL, "fatal");
    CHECK(writes == 0, "disabled threshold suppresses normal log output");

    set_log_level(LOG_ALL);
    reenter = 1;
    pr_log(true, LOG_INFO, "OUTER123");
    CHECK(captured_len == 8 && memcmp(captured, "OUTER123", 8) == 0,
          "nested logging cannot overwrite the caller's message");

    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    pr_log(true, LOG_INFO, "%s", long_message);
    CHECK(captured_len <= CONFIG_LOG_MESSAGE_SIZE - 1,
          "message respects configured formatting capacity");

    if (failures != 0) {
        fprintf(stderr, "%d printk checks failed\n", failures);
        return 1;
    }
    puts("printk checks passed");
    return 0;
}
