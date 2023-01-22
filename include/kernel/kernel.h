/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_KERNEL_H__
#define __NOS_KERNEL_H__

#include <kernel/types.h>
#include <kernel/compiler.h>
#include <kernel/section.h>
#include <kernel/errno.h>
#include <autocfg.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

#define BUG_ON(ex)

#endif /* __NOS_KERNEL_H__ */
