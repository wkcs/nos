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

int nos_start(void)
{
    cpu_init();
    arch_init();
    board_init();
    console_init();
    pr_info("console init\r\n");
    memblock_early_init();
    mm_init();
    pid_init();
    while (1) {
        sleep(1);
        pid_t pid = pid_alloc();
        pr_info("test...%u\r\n", pid_alloc());
        pid_free(pid);
    }

    return 0;
}
