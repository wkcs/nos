/**
 * Copyright (C) 2026 Nick Hu
 *
 * Exercise real console dispatch with controlled device callbacks.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../include/kernel/console.h"
#include "../../kernel/console.c"

addr_t __console_data_start;
addr_t __console_data_end;

static int read_byte(void)
{
    return 255;
}

#ifdef NOS_CONSOLE_PANIC_API
static int panic_started;
static int panic_writes;
static int record_writes;

static int write_record(const char *buf, int len)
{
    (void)buf;
    record_writes++;
    return len;
}

static int start_panic(void)
{
    panic_started++;
    return 0;
}

static int write_panic(const char *buf, int len)
{
    (void)buf;
    panic_writes++;
    return len;
}
#endif

static int write_partial(const char *buf, int len)
{
    (void)buf;
    return len > 2 ? 2 : len;
}

static int check_missing_read(void)
{
    pid_t child = fork();
    int status;

    if (child == 0) {
        struct console_ops ops = { 0 };
        struct console console = { .name = "test", .ops = &ops };
        g_console = &console;
        _exit(console_getc() == -ENOSYS ? 0 : 1);
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
    struct console_ops ops = { .write = write_partial, .getc = read_byte,
#ifdef NOS_CONSOLE_PANIC_API
        .write_record = write_record, .panic_begin = start_panic,
        .panic_write = write_panic,
#endif
    };
    struct console console = { .name = "test", .ops = &ops };
    int failures = 0;

    g_console = NULL;
    if (console_getc() != -ENODEV) {
        fputs("FAIL: absent console reports ENODEV\n", stderr);
        failures++;
    }
    g_console = &console;
    if (console_getc() != 255) {
        fputs("FAIL: input preserves 0xff as a byte\n", stderr);
        failures++;
    }
    if (console_write("hello", 5) != 2) {
        fputs("FAIL: console reports short writes\n", stderr);
        failures++;
    }
    if (!check_missing_read()) {
        fputs("FAIL: missing input callback reports ENOSYS\n", stderr);
        failures++;
    }
#ifdef NOS_CONSOLE_PANIC_API
    if (console_write_record("hello", 5) != 5 || record_writes != 1 ||
        console_panic_begin() != 0 || panic_started != 1 ||
        console_write("hello", 5) != 5 || panic_writes != 1) {
        fputs("FAIL: records and panic must select their own transport hooks\n", stderr);
        failures++;
    }
#else
    fputs("FAIL: record and panic console transport is unavailable\n", stderr);
    failures++;
#endif
    if (failures != 0) return 1;
    puts("console dispatch checks passed");
    return 0;
}
