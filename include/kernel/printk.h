/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_PRINTK_H__
#define __NOS_PRINTK_H__

#include <kernel/kernel.h>
#include <stdarg.h>
#include <stdbool.h>

enum log_level {
  LOG_ALL = 0,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
  LOG_DISABLE,
};

#define NOS_LOG_RECORD_RING 1
#define NOS_LOG_METADATA_API 1
#define NOS_LOG_VECTOR_API 1
#define NOS_LOG_CLOCK_READY_API 1
#define LOG_RECORD_UNTAGGED (1U << 0)

/* Internal record header; timestamp wraps modulo 2^32 milliseconds. */
struct kernel_log_record {
    uint32_t timestamp_ms;
    uint16_t length;
    uint8_t level;
    uint8_t flags;
};

struct kernel_log_stats {
    uint32_t dropped_records;
    uint32_t truncated_records;
    uint32_t high_water;
};

typedef void (*kernel_log_observer_fn)(const struct kernel_log_record *record,
                                       const void *first, unsigned int first_len,
                                       const void *second, unsigned int second_len,
                                       void *priv);

__printf(3, 4) int pr_log(bool no_tag, enum log_level level, const char *fmt,
                          ...);
void set_log_level(enum log_level level);
int vprintk(const char *fmt, va_list args);
void kernel_log_init(void);
void kernel_log_clock_ready(void);
unsigned int kernel_log_write(const void *buf, unsigned int len);
unsigned int kernel_log_write_record(const void *buf, unsigned int len,
                                     enum log_level level, uint8_t flags,
                                     uint32_t timestamp_ms);
/* Consume one complete queued record; the console drain is another consumer. */
unsigned int kernel_log_read(void *buf, unsigned int len);
unsigned int kernel_log_read_record(void *buf, unsigned int len,
                                    struct kernel_log_record *record);
void kernel_log_flush(void);
void kernel_log_panic_flush(void);
void kernel_log_record_truncated(void);
void kernel_log_get_stats(struct kernel_log_stats *stats);
void kernel_log_set_observer(kernel_log_observer_fn fn, void *priv);

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#ifndef CONFIG_LOG_COMPILE_LEVEL
#define CONFIG_LOG_COMPILE_LEVEL 0
#endif

/* A constant condition also removes disabled call-site argument evaluation. */
#define __pr_log(no_tag, level, fmt, ...)                                      \
    ((level) >= CONFIG_LOG_COMPILE_LEVEL ?                                     \
     pr_log(no_tag, level, pr_fmt(fmt), ##__VA_ARGS__) : 0)

#define pr_fatal(fmt, ...) __pr_log(false, LOG_FATAL, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) __pr_log(false, LOG_ERROR, fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) __pr_log(false, LOG_WARNING, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) __pr_log(false, LOG_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) __pr_log(false, LOG_DEBUG, fmt, ##__VA_ARGS__)

#define pr_fatal_no_tag(fmt, ...) __pr_log(true, LOG_FATAL, fmt, ##__VA_ARGS__)
#define pr_err_no_tag(fmt, ...) __pr_log(true, LOG_ERROR, fmt, ##__VA_ARGS__)
#define pr_warning_no_tag(fmt, ...) __pr_log(true, LOG_WARNING, fmt, ##__VA_ARGS__)
#define pr_info_no_tag(fmt, ...) __pr_log(true, LOG_INFO, fmt, ##__VA_ARGS__)
#define pr_debug_no_tag(fmt, ...) __pr_log(true, LOG_DEBUG, fmt, ##__VA_ARGS__)

void set_log_enabled(bool enable);

#endif /* __NOS_PRINTK_H__ */
