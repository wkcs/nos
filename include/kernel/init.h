/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_INIT_H__
#define __NOS_INIT_H__

#include <asm/init.h>
#include <board/init.h>
#include <kernel/section.h>

typedef struct {
    char *name;
    int (*task_init_func)(void);
} init_task_t;

#define task_init(fn) \
    __visible __task const init_task_t __maybe_unused __int_##fn = { \
            .name = #fn, \
            .task_init_func = fn \
        }

int idel_task_init(void);

#endif /* __NOS_INIT_H__ */
