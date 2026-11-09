/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 进程级文件描述符表与 dup/dup2 机制单元测试
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fs/vfs.h>
#include <kernel/task.h>
#include "../../fs/vfs_fd.h"

/* 测试用当前任务全局指针，覆盖 weak 的 task_current() */
static struct task_struct *s_current_task = NULL;

struct task_struct *task_current(void)
{
    return s_current_task;
}

static int s_release_count = 0;

static int mock_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    s_release_count++;
    return 0;
}

static const struct file_operations s_test_fops = {
    .release = mock_release,
};

static struct file *alloc_mock_file(int marker)
{
    struct file *f = calloc(1, sizeof(*f));
    assert(f != NULL);
    f->f_op = &s_test_fops;
    f->marker = marker;
    return f;
}

int nos_dup(int oldfd)
{
    return vfs_fd_dup(oldfd);
}

int nos_dup2(int oldfd, int newfd)
{
    return vfs_fd_dup2(oldfd, newfd);
}

static void test_early_fallback(void)
{
    s_current_task = NULL;
    s_release_count = 0;

    /* task_current() == NULL 时应使用 early_files 回退表 */
    int fd0 = vfs_fd_reserve();
    assert(fd0 == 0);

    struct file *f0 = alloc_mock_file(100);
    vfs_fd_publish(fd0, f0);

    struct file *got = vfs_fd_get(fd0);
    assert(got == f0);
    assert(f0->f_refs == 2);
    vfs_fd_put(got);
    assert(f0->f_refs == 1);

    int dup_fd = nos_dup(fd0);
    assert(dup_fd == 1);
    assert(f0->f_refs == 2);

    assert(vfs_fd_close(fd0) == 0);
    assert(s_release_count == 0);
    assert(f0->f_refs == 1);

    assert(vfs_fd_close(dup_fd) == 0);
    assert(s_release_count == 1);
}

static void test_per_task_isolation(void)
{
    struct task_struct task_a;
    struct task_struct task_b;
    vfs_task_files_init(&task_a, NULL);
    vfs_task_files_init(&task_b, NULL);
    s_release_count = 0;

    /* 任务 A 打开描述符 0 */
    s_current_task = &task_a;
    int fd_a = vfs_fd_reserve();
    assert(fd_a == 0);
    struct file *fa = alloc_mock_file(201);
    vfs_fd_publish(fd_a, fa);
    assert(task_a.files[0] == fa);

    /* 切换到任务 B，任务 B 的描述符 0 应当为空 */
    s_current_task = &task_b;
    assert(vfs_fd_get(0) == NULL);
    int fd_b = vfs_fd_reserve();
    assert(fd_b == 0);
    struct file *fb = alloc_mock_file(202);
    vfs_fd_publish(fd_b, fb);
    assert(task_b.files[0] == fb);

    /* 切换回任务 A，验证任务 A 与任务 B 相互隔离 */
    s_current_task = &task_a;
    struct file *check_a = vfs_fd_get(0);
    assert(check_a == fa);
    vfs_fd_put(check_a);

    s_current_task = &task_b;
    struct file *check_b = vfs_fd_get(0);
    assert(check_b == fb);
    vfs_fd_put(check_b);

    /* 清理两个任务 */
    vfs_task_files_cleanup(&task_a);
    vfs_task_files_cleanup(&task_b);
    assert(s_release_count == 2);
}

static void test_dup_and_dup2(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;
    s_release_count = 0;

    struct file *f0 = alloc_mock_file(300);
    int fd0 = vfs_fd_reserve();
    assert(fd0 == 0);
    vfs_fd_publish(fd0, f0);

    /* 测试 nos_dup：分配最低可用描述符 1 */
    int fd1 = nos_dup(fd0);
    assert(fd1 == 1);
    assert(task.files[1] == f0);
    assert(f0->f_refs == 2);

    /* 测试 nos_dup 错误分支 */
    assert(nos_dup(-1) == -EBADF);
    assert(nos_dup(2) == -EBADF);
    assert(nos_dup(CONFIG_TASK_MAX_FDS) == -EBADF);

    /* 测试 nos_dup2(oldfd, newfd) 其中 oldfd == newfd */
    assert(nos_dup2(fd0, fd0) == fd0);
    assert(f0->f_refs == 2);
    assert(nos_dup2(5, 5) == -EBADF); /* 未打开的描述符自复制应报错 */

    /* 测试 nos_dup2 重定向到指定未打开描述符（如 3） */
    int fd3 = nos_dup2(fd0, 3);
    assert(fd3 == 3);
    assert(task.files[3] == f0);
    assert(f0->f_refs == 3);

    /* 打开描述符 2 作为目标，随后用 dup2(fd0, 2) 替换它 */
    struct file *f2 = alloc_mock_file(302);
    int fd2 = vfs_fd_reserve();
    assert(fd2 == 2);
    vfs_fd_publish(fd2, f2);
    assert(f2->f_refs == 1);

    /* 此时描述符 2 已打开 f2，dup2 覆盖它应安全释放 f2 */
    assert(nos_dup2(fd0, 2) == 2);
    assert(task.files[2] == f0);
    assert(f0->f_refs == 4);
    assert(s_release_count == 1); /* f2 已被释放 */

    /* 测试同一个文件的 dup2(0, 1) 覆盖（oldfd != newfd 但指向同一文件） */
    assert(nos_dup2(0, 1) == 1);
    assert(task.files[1] == f0);
    assert(f0->f_refs == 4); /* 引用计数保持不变 */
    assert(s_release_count == 1);

    /* 参数越界测试 */
    assert(nos_dup2(-1, 0) == -EBADF);
    assert(nos_dup2(0, -1) == -EBADF);
    assert(nos_dup2(0, CONFIG_TASK_MAX_FDS) == -EBADF);
    assert(nos_dup2(CONFIG_TASK_MAX_FDS, 0) == -EBADF);

    /* 耗尽描述符测试 nos_dup 返回 -EMFILE */
    for (int i = 4; i < CONFIG_TASK_MAX_FDS; i++) {
        assert(nos_dup2(fd0, i) == i);
    }
    assert(f0->f_refs == (unsigned int)CONFIG_TASK_MAX_FDS);
    assert(nos_dup(fd0) == -EMFILE);

    /* 清理该任务 */
    vfs_task_files_cleanup(&task);
    assert(s_release_count == 2); /* f0 也被释放 */
}

static void test_task_inheritance_and_exit(void)
{
    struct task_struct parent;
    struct task_struct child;
    vfs_task_files_init(&parent, NULL);
    s_current_task = &parent;
    s_release_count = 0;

    /* 父任务打开 fd 0 与 fd 2 */
    int fd0 = vfs_fd_reserve();
    assert(fd0 == 0);
    struct file *f0 = alloc_mock_file(400);
    vfs_fd_publish(fd0, f0);

    /* 预留并取消 fd 1 */
    int fd1 = vfs_fd_reserve();
    assert(fd1 == 1);
    vfs_fd_cancel(fd1);

    int fd2 = vfs_fd_reserve();
    assert(fd2 == 1); /* 0 已占，1 已取消，故分配 1 */
    struct file *f1 = alloc_mock_file(401);
    vfs_fd_publish(fd2, f1);

    assert(f0->f_refs == 1);
    assert(f1->f_refs == 1);

    /* 子任务继承父任务的文件描述符表 */
    vfs_task_files_init(&child, &parent);
    assert(child.files[0] == f0);
    assert(child.files[1] == f1);
    assert(child.files[2] == NULL);
    assert(f0->f_refs == 2);
    assert(f1->f_refs == 2);

    /* 子任务关闭 fd 0，父任务不受影响 */
    s_current_task = &child;
    assert(vfs_fd_close(0) == 0);
    assert(child.files[0] == NULL);
    assert(parent.files[0] == f0);
    assert(f0->f_refs == 1);
    assert(s_release_count == 0);

    /* 子任务退出释放 */
    vfs_task_files_cleanup(&child);
    assert(f1->f_refs == 1);
    assert(s_release_count == 0);

    /* 父任务退出释放全部文件 */
    s_current_task = &parent;
    vfs_task_files_cleanup(&parent);
    assert(s_release_count == 2);

    /* 再次 cleanup 安全性验证 (幂等) */
    vfs_task_files_cleanup(&parent);
    assert(s_release_count == 2);
}

int main(void)
{
    test_early_fallback();
    test_per_task_isolation();
    test_dup_and_dup2();
    test_task_inheritance_and_exit();
    return 0;
}
