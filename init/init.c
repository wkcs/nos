/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/init.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/task.h>

extern addr_t __task_data_start;
extern addr_t __task_data_end;

static u32 get_init_task_num(void)
{
    addr_t addr_size = (addr_t)&__task_data_end - (addr_t)&__task_data_start;

    if (addr_size == 0) {
        return 0;
    }
    if (addr_size % sizeof(init_task_t) != 0) {
        return 0;
    }

    return (addr_size / sizeof(init_task_t));
}

static init_task_t *find_first_init_task(void)
{
    addr_t start_addr = (addr_t)&__task_data_start;
    return (init_task_t *)start_addr;
}

void task_init_call(void)
{
    int rc;
    u32 i;
    u32 int_task_num = get_init_task_num();
    init_task_t *init_task_info = find_first_init_task();

    for(i = 0; i < int_task_num; i++) {
        if (init_task_info->task_init_func == NULL) {
            pr_err("%s: init func is NULL\r\n", init_task_info->name);
            continue;
        }

        pr_info("%s: start init\r\n", init_task_info->name);
        rc = init_task_info->task_init_func();
        if (rc != 0) {
            pr_err("%s: init error\r\n", init_task_info->name);
            continue;
        }
    }
}

static void idel_task_entry(void* parameter)
{
    while (1) {
        sleep(1);
        pr_info("%s: test...\r\n", current->name);
    }
}

int idel_task_init(void)
{
    struct task_struct *idel;

    idel = task_create("idel", idel_task_entry, NULL, CONFIG_MAX_PRIORITY - 1, 10, NULL);
    if (idel == NULL) {
        pr_fatal("creat idle task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    pr_info("idel=0x%p\r\n", idel);
    task_ready(idel);

    return 0;
}
