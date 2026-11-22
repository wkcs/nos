/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <unistd.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_UNISTD_H__
#define __NOS_FREESTANDING_UNISTD_H__

#include <sys/types.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

int isatty(int fd);

#endif /* __NOS_FREESTANDING_UNISTD_H__ */
