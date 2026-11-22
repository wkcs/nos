/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <errno.h> 头文件（防循环包含）。
 */

#ifndef __NOS_FREESTANDING_ERRNO_H__
#define __NOS_FREESTANDING_ERRNO_H__

#ifndef __NOS_UAPI_ERRNO_H__
#include <uapi/nos/errno.h>
#endif

extern int *__nos_errno_location(void);

#ifndef errno
#define errno (*__nos_errno_location())
#endif

#endif /* __NOS_FREESTANDING_ERRNO_H__ */
