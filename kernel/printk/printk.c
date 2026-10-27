/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Bounded, reentrant kernel logging frontend.
 */

#include <kernel/cpu.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <lib/vsprintf.h>

#ifndef CONFIG_LOG_MESSAGE_SIZE
#define CONFIG_LOG_MESSAGE_SIZE 128
#endif

#ifdef CONFIG_DEFAULT_LOG_LEVEL
static enum log_level g_log_level = CONFIG_DEFAULT_LOG_LEVEL;
#else
static enum log_level g_log_level = LOG_INFO;
#endif
static bool g_log_enabled = true;
static bool g_log_clock_ready;

void kernel_log_clock_ready(void)
{
    __atomic_store_n(&g_log_clock_ready, true, __ATOMIC_RELAXED);
}

void set_log_enabled(bool enable)
{
    __atomic_store_n(&g_log_enabled, enable, __ATOMIC_RELAXED);
}

void set_log_level(enum log_level level)
{
    if (level < LOG_ALL || level > LOG_DISABLE) {
        return;
    }
    __atomic_store_n(&g_log_level, level, __ATOMIC_RELAXED);
}

static const char *log_level_name(enum log_level level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO: return "INFO";
    case LOG_WARNING: return "WARNING";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default: return NULL;
    }
}

__printf(3, 4) int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    char buf[CONFIG_LOG_MESSAGE_SIZE];
    const char *name = log_level_name(level);
    size_t used = 0;
    bool truncated = false;
    uint64_t timestamp = 0;
    int len;
    va_list args;

    if (name == NULL || fmt == NULL ||
        level < __atomic_load_n(&g_log_level, __ATOMIC_RELAXED) ||
        (!__atomic_load_n(&g_log_enabled, __ATOMIC_RELAXED) && level < LOG_ERROR)) {
        return 0;
    }
    if (__atomic_load_n(&g_log_clock_ready, __ATOMIC_RELAXED)) {
        timestamp = cpu_run_time_us();
    }
    if (!no_tag) {
        uint32_t seconds = (uint32_t)(timestamp / 1000000U);
        uint32_t useconds = (uint32_t)(timestamp % 1000000U);

        len = snprintf(buf, sizeof(buf), "%06u.%06u[%s]", seconds, useconds, name);
        if (len < 0) {
            return len;
        }
        if ((size_t)len >= sizeof(buf)) {
            used = sizeof(buf) - 1;
            truncated = true;
        } else {
            used = (size_t)len;
        }
    }

    va_start(args, fmt);
    len = vsnprintf(buf + used, sizeof(buf) - used, fmt, args);
    va_end(args);
    if (len < 0) {
        return len;
    }
    if ((size_t)len >= sizeof(buf) - used) {
        used = sizeof(buf) - 1;
        truncated = true;
    } else {
        used += (size_t)len;
    }
    if (truncated) {
        kernel_log_record_truncated();
    }
    return (int)kernel_log_write_record(buf, (unsigned int)used, level,
                                        no_tag ? LOG_RECORD_UNTAGGED : 0,
                                        (uint32_t)(timestamp / 1000U));
}

int vprintk(const char *fmt, va_list args)
{
    char buf[CONFIG_LOG_MESSAGE_SIZE];
    int len;
    size_t used;

    if (fmt == NULL) {
        return 0;
    }
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len < 0) {
        return len;
    }
    used = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
    if ((size_t)len >= sizeof(buf)) {
        kernel_log_record_truncated();
    }
    return (int)kernel_log_write_record(buf, (unsigned int)used, LOG_FATAL,
                                        LOG_RECORD_UNTAGGED, 0);
}
