/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/printk.h>
#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <lib/vsprintf.h>
#include <lib/string.h>

#ifdef CONFIG_DEFAULT_LOG_LEVEL
static enum log_level g_log_level = CONFIG_DEFAULT_LOG_LEVEL;
#else
static enum log_level g_log_level = LOG_INFO;
#endif

#ifdef CONFIG_LOG_BUF_SIZE
static char log_buf[CONFIG_LOG_BUF_SIZE];
#else
static char log_buf[256];
#endif

__printf(2,3) int pr_log(enum log_level level, const char *fmt, ...)
{
    va_list args;
    char *buf = log_buf;
    uint32_t time_sec, time_usec;
    int len;

    if (level > g_log_level) {
        time_sec = (uint32_t)(cpu_run_time_us() / 1000000);
        time_usec = (uint32_t)(cpu_run_time_us() % 1000000);

        switch(level) {
            case LOG_FATAL:
                sprintf(buf, "%06d.%06d[FATAL]",  time_sec, time_usec);
                len = 20;
                break;
            case LOG_ERROR:
                sprintf(buf, "%06d.%06d[ERROR]",  time_sec, time_usec);
                len = 20;
                break;
            case LOG_WARNING:
                sprintf(buf, "%06d.%06d[WARNING]",  time_sec, time_usec);
                len = 22;
                break;
            case LOG_INFO:
                sprintf(buf, "%06d.%06d[INFO]",  time_sec, time_usec);
                len = 19;
                break;
            case LOG_DEBUG:
                sprintf(buf, "%06d.%06d[DEBUG]",  time_sec, time_usec);
                len = 20;
                break;
            default:
                return 0;
        }

        va_start(args, fmt);
        len += vsprintf(buf + len, fmt, args);
        va_end(args);

        len = console_write(buf, len);

        return len;
    }

    return 0;
}

void set_log_level(enum log_level level)
{
    g_log_level = level;
}
