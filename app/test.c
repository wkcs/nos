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

static void test1_task_entry(void* parameter)
{
    u32 usage;
    u32 all_usage;

    while (1) {
        msleep(500);
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
    }
}

static int test1_task_init(void)
{
    struct task_struct *test1;

    test1 = task_create("test1", test1_task_entry, NULL, 5, 10, NULL);
    if (test1 == NULL) {
        pr_fatal("creat test1 task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(test1);

    return 0;
}
task_init(test1_task_init);

static void test2_task_entry(void* parameter)
{
    u32 usage;
    u32 all_usage;

    while (1) {
        msleep(800);
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
    }
}

static int test2_task_init(void)
{
    struct task_struct *test2;

    test2 = task_create("test2", test2_task_entry, NULL, 5, 10, NULL);
    if (test2 == NULL) {
        pr_fatal("creat test2 task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(test2);

    return 0;
}
task_init(test2_task_init);

static void test3_task_entry(void* parameter)
{
    u32 usage;
    u32 all_usage;

    while (1) {
        msleep(600);
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
    }
}

static int test3_task_init(void)
{
    struct task_struct *test3;

    test3 = task_create("test3", test3_task_entry, NULL, 15, 10, NULL);
    if (test3 == NULL) {
        pr_fatal("creat test3 task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(test3);

    return 0;
}
task_init(test3_task_init);

static void test4_task_entry(void* parameter)
{
    u32 usage;
    u32 all_usage;

    while (1) {
        sleep(3);
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
    }
}

static int test4_task_init(void)
{
    struct task_struct *test4;

    test4 = task_create("test4", test4_task_entry, NULL, 25, 10, NULL);
    if (test4 == NULL) {
        pr_fatal("creat test4 task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(test4);

    return 0;
}
task_init(test4_task_init);

static void test5_task_entry(void* parameter)
{
    u32 usage;
    u32 all_usage;

    while (1) {
        sleep(2);
        usage = task_get_cpu_usage(current) / 100;
        all_usage = get_cpu_usage() / 100;
        pr_info("%s: usage:%u.%02u%%, total:%u.%02u%%\r\n", current->name,
                usage / 100, usage % 100, all_usage / 100, all_usage % 100);
    }
}

static int test5_task_init(void)
{
    struct task_struct *test5;

    test5 = task_create("test5", test5_task_entry, NULL, 68, 10, NULL);
    if (test5 == NULL) {
        pr_fatal("creat test5 task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(test5);

    return 0;
}
task_init(test5_task_init);