/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <kernel/init.h>
#include <kernel/printk.h>

extern const struct sys_init_entry __sys_init_start[];
extern const struct sys_init_entry __sys_init_end[];

void sys_init_run(enum init_level level)
{
    const struct sys_init_entry *entry;

    if (level >= INIT_LEVEL_COUNT) {
        pr_err("SYS_INIT: invalid init level\n");
        return;
    }

    for (entry = __sys_init_start; entry < __sys_init_end; entry++) {
        int ret;

        if (entry->level != level) {
            continue;
        }

        ret = entry->init();
        if (ret) {
            pr_err("SYS_INIT: init failed (%d)\n", ret);
        }
    }
}

static int sys_init_test(void)
{
    pr_info("sys_init ok\n");
    return 0;
}

SYS_INIT(sys_init_test, INIT_LEVEL_APPLICATION);
