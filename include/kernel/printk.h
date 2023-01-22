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

__printf(2,3) int pr_log(enum log_level level, const char *fmt, ...);
void set_log_level(enum log_level level);

#define pr_fatal(fmt, ...)  \
    pr_log(LOG_FATAL, fmt, ## __VA_ARGS__)

#define pr_err(fmt, ...)  \
    pr_log(LOG_ERROR, fmt, ## __VA_ARGS__)

#define pr_warning(fmt, ...)  \
    pr_log(LOG_WARNING, fmt, ## __VA_ARGS__)

#define pr_info(fmt, ...)  \
    pr_log(LOG_INFO, fmt, ## __VA_ARGS__)

#define pr_debug(fmt, ...)  \
    pr_log(LOG_DEBUG, fmt, ## __VA_ARGS__)

#endif /* __NOS_PRINTK_H__ */
