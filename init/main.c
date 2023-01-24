/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/sleep.h>
#include <kernel/mm.h>
#include <kernel/sch.h>

int nos_start(void)
{
    cpu_init();
    arch_init();
    board_init();
    console_init();
    pr_info("console init\r\n");
    mm_node_early_init();
    mm_init();
    pid_init();

    sch_init();
    task_init_call();
    sch_start();

    return 0;
}
