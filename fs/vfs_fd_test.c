/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/task.h>
#include "vfs_fd.h"

#define VFS_FD_TEST_READS 32

static int test_fd;
static unsigned int seen[VFS_FD_TEST_READS * 2];
static unsigned int completed;
static unsigned int failed;
static struct inode test_inode;

static ssize_t test_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
    loff_t start = *pos;
    (void)file;
    if (count == 0 || start < 0 || start >= VFS_FD_TEST_READS * 2) return -EINVAL;
    msleep(CONFIG_SYS_TICK_MS);
    buf[0] = (char)start;
    *pos = start + 1;
    return 1;
}

static const struct file_operations test_ops = { .read = test_read };

static void test_worker(void *arg)
{
    unsigned int i;
    (void)arg;
    for (i = 0; i < VFS_FD_TEST_READS; i++) {
        char value = 0;
        int rc = nos_read(test_fd, &value, 1);
        unsigned int index = (unsigned char)value;
        if (rc != 1 || index >= VFS_FD_TEST_READS * 2) {
            __atomic_store_n(&failed, 1U, __ATOMIC_RELAXED);
            break;
        }
        __atomic_add_fetch(&seen[index], 1U, __ATOMIC_RELAXED);
    }
    if (__atomic_add_fetch(&completed, 1U, __ATOMIC_ACQ_REL) == 2U) {
        for (i = 0; i < VFS_FD_TEST_READS * 2; i++) {
            if (__atomic_load_n(&seen[i], __ATOMIC_RELAXED) != 1U) failed = 1U;
        }
        if (nos_close(test_fd) != 0) failed = 1U;
        if (failed) pr_err("vfs concurrent position FAILED\n");
        else pr_info("vfs concurrent position OK\n");
    }
}

static int vfs_fd_test_start(void)
{
    struct task_struct *first, *second;
    struct file *file = kzalloc(sizeof(*file), GFP_KERNEL);
    int fd;

    if (file == NULL) return -ENOMEM;
    fd = vfs_fd_reserve();
    if (fd < 0) { kfree(file); return fd; }
    file->f_inode = &test_inode;
    file->f_op = &test_ops;
    file->f_flags = O_RDONLY;
    vfs_fd_publish(fd, file);
    test_fd = fd;
    first = task_create("vfs_test_a", test_worker, NULL, 5, 1024, 10, NULL);
    second = task_create("vfs_test_b", test_worker, NULL, 5, 1024, 10, NULL);
    if (first == NULL || second == NULL) {
        if (first != NULL) task_del(first);
        if (second != NULL) task_del(second);
        nos_close(fd);
        return -ENOMEM;
    }
    task_ready(first);
    task_ready(second);
    return 0;
}
task_init(vfs_fd_test_start);
