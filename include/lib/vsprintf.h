/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __VSPRINTF_H__
#define __VSPRINTF_H__

#include <kernel/kernel.h>
#include <kernel/types.h>
#include <stdarg.h>

__printf(3, 0) uint32_t vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(3, 4) uint32_t snprintf(char *buf, size_t size, const char *fmt, ...);
__printf(2, 0) uint32_t vsprintf(char *buf, const char *format, va_list arg_ptr);
__printf(2, 3) uint32_t sprintf(char *buf, const char *format, ...);

#endif