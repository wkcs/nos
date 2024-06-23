/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_PID_H__
#define __NOS_PID_H__

#include <kernel/kernel.h>

#ifndef _PID_T_DECLARED
typedef u32 pid_t;
#define _PID_T_DECLARED
#endif

pid_t pid_alloc(void);
int pid_free(pid_t pid);
void pid_init(void);

#endif /* __NOS_PID_H__ */
