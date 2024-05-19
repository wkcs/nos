/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/init.h>
#include <kernel/sleep.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>
#include <kernel/msg_queue.h>
#include <usb/usb_device.h>

static void keyboard_task_entry(void* parameter)
{
    __maybe_unused u32 usage;
    __maybe_unused u32 all_usage;

    while (1) {
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
        sleep(1);
    }
}

struct task_struct *keyboard_task;
static int keyboard_task_init(void)
{
    keyboard_task = task_create("keyboard", keyboard_task_entry, NULL, 23, 2048, 10, NULL);
    if (keyboard_task == NULL) {
        pr_fatal("creat keyboard task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(keyboard_task);

    return 0;
}
task_init(keyboard_task_init);
