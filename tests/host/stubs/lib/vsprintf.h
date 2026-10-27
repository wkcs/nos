/**
 * Copyright (C) 2026 Nick Hu
 *
 * Formatter declarations used by host regression tests.
 */

#ifndef NOS_TEST_VSPRINTF_H
#define NOS_TEST_VSPRINTF_H

#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list args);
int sprintf(char *buf, const char *fmt, ...);

#endif
