/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <sys/types.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_SYS_TYPES_H__
#define __NOS_FREESTANDING_SYS_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <kernel/types.h>

#ifndef _OFF_T_DECLARED
typedef long long off_t;
#define _OFF_T_DECLARED
#endif

#ifndef _MODE_T_DECLARED
typedef unsigned int mode_t;
#define _MODE_T_DECLARED
#endif

#ifndef _DEV_T_DECLARED
typedef unsigned int dev_t;
#define _DEV_T_DECLARED
#endif

#endif /* __NOS_FREESTANDING_SYS_TYPES_H__ */
