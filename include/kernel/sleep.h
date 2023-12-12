/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SLEEP_H__
#define __NOS_SLEEP_H__

#include <kernel/types.h>

void sleep(u32 sec);
void msleep(u32 msec);
void usleep(u32 usec);
void nsleep(u32 nsec);

#endif /* __NOS_SLEEP_H__ */
