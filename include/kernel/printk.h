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

enum log_level {
    LOG_ALL = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL,
    LOG_DISABLE,
};

__printf(3,4) int pr_log(bool no_tag, enum log_level level, const char *fmt, ...);
void set_log_level(enum log_level level);

#define pr_fatal(fmt, ...)  \
    pr_log(false, LOG_FATAL, fmt, ## __VA_ARGS__)

#define pr_err(fmt, ...)  \
    pr_log(false, LOG_ERROR, fmt, ## __VA_ARGS__)

#define pr_warning(fmt, ...)  \
    pr_log(false, LOG_WARNING, fmt, ## __VA_ARGS__)

#define pr_info(fmt, ...)  \
    pr_log(false, LOG_INFO, fmt, ## __VA_ARGS__)

#define pr_debug(fmt, ...)  \
    pr_log(false, LOG_DEBUG, fmt, ## __VA_ARGS__)

#define pr_fatal_no_tag(fmt, ...)  \
    pr_log(true, LOG_FATAL, fmt, ## __VA_ARGS__)

#define pr_err_no_tag(fmt, ...)  \
    pr_log(true, LOG_ERROR, fmt, ## __VA_ARGS__)

#define pr_warning_no_tag(fmt, ...)  \
    pr_log(true, LOG_WARNING, fmt, ## __VA_ARGS__)

#define pr_info_no_tag(fmt, ...)  \
    pr_log(true, LOG_INFO, fmt, ## __VA_ARGS__)

#define pr_debug_no_tag(fmt, ...)  \
    pr_log(true, LOG_DEBUG, fmt, ## __VA_ARGS__)

#endif /* __NOS_PRINTK_H__ */
