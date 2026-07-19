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

enum init_level {
    INIT_LEVEL_EARLY,
    INIT_LEVEL_PRE_KERNEL_1,
    INIT_LEVEL_PRE_KERNEL_2,
    INIT_LEVEL_POST_KERNEL,
    INIT_LEVEL_APPLICATION,
    INIT_LEVEL_COUNT,
};

typedef int (*sys_init_entry_t)(void);

struct sys_init_entry {
    sys_init_entry_t init;
    enum init_level level;
};

#define SYS_INIT(fn, init_level) \
    static const struct sys_init_entry __sys_init_##fn __sys_init = { \
        .init = fn, \
        .level = init_level, \
    }

#define task_init(fn) \
    __visible __task const init_task_t __maybe_unused __int_##fn = { \
            .name = #fn, \
            .task_init_func = fn \
        }

void sys_init_run(enum init_level level);

int idel_task_init(void);

#endif /* __NOS_INIT_H__ */
