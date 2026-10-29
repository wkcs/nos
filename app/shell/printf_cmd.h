/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PRINTF_CMD_H
#define NOS_SHELL_PRINTF_CMD_H
#include <stddef.h>
typedef int (*shell_printf_write_fn)(void *context, const char *data, size_t length);
int shell_printf_execute(const char *format, int argc, char *const argv[],
                         shell_printf_write_fn write, void *context,
                         char *buffer, size_t buffer_size);
#endif
