/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <stdio.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_STDIO_H__
#define __NOS_FREESTANDING_STDIO_H__

#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
#define EOF (-1)
#endif

#ifndef BUFSIZ
#define BUFSIZ 256
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct __nos_freestanding_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list args);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int puts(const char *s);
int putchar(int c);
int getchar(void);
int fputs(const char *s, FILE *stream);
int fputc(int c, FILE *stream);

#endif /* __NOS_FREESTANDING_STDIO_H__ */
