/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <limits.h>
#include "vfs_fd.h"

#ifndef CONFIG_TASK_MAX_FDS
#define CONFIG_TASK_MAX_FDS 8
#endif

/* 引导早期或无调度器上下文时的回退描述符表 */
static struct file *early_files[CONFIG_TASK_MAX_FDS];

/* 预留占位符（区分已占用但未发布的描述符槽位） */
static struct file vfs_reserved_marker;
#define VFS_FD_RESERVED (&vfs_reserved_marker)

static SPINLOCK(open_files_lock);

/* 获取当前任务的文件描述符表；若当前无任务上下文则回退至 early_files */
static struct file **vfs_get_files_table(void)
{
    struct task_struct *curr = task_current();
    if (curr != NULL) {
        return curr->files;
    }
    return early_files;
}

static int vfs_fd_destroy(struct file *file)
{
    int rc = 0;
    if (file->f_op != NULL && file->f_op->release != NULL)
        rc = file->f_op->release(file->f_inode, file);
    if (file->f_path.dentry != NULL) vfs_dentry_put(file->f_path.dentry);
    kfree(file);
    return rc;
}

int vfs_fd_reserve(void)
{
    int fd;
    struct file **table;

    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    for (fd = 0; fd < CONFIG_TASK_MAX_FDS; fd++) {
        if (table[fd] == NULL) {
            table[fd] = VFS_FD_RESERVED;
            spin_unlock(&open_files_lock);
            return fd;
        }
    }
    spin_unlock(&open_files_lock);
    return -EMFILE;
}

void vfs_fd_cancel(int fd)
{
    struct file **table;

    if (fd < 0 || fd >= CONFIG_TASK_MAX_FDS) return;
    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    if (table[fd] == VFS_FD_RESERVED) {
        table[fd] = NULL;
    }
    spin_unlock(&open_files_lock);
}

void vfs_fd_publish(int fd, struct file *file)
{
    struct file **table;

    if (fd < 0 || fd >= CONFIG_TASK_MAX_FDS) return;
    mutex_init(&file->f_pos_lock);
    spin_lock(&open_files_lock);
    file->f_refs = 1;
    table = vfs_get_files_table();
    table[fd] = file;
    spin_unlock(&open_files_lock);
}

struct file *vfs_fd_get(int fd)
{
    struct file *file = NULL;
    struct file **table;

    if (fd < 0 || fd >= CONFIG_TASK_MAX_FDS) return NULL;
    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    if (table[fd] != NULL && table[fd] != VFS_FD_RESERVED &&
        table[fd]->f_refs != UINT_MAX) {
        file = table[fd];
        file->f_refs++;
    }
    spin_unlock(&open_files_lock);
    return file;
}

int vfs_fd_put(struct file *file)
{
    bool destroy;
    if (file == NULL || file == VFS_FD_RESERVED) return 0;
    spin_lock(&open_files_lock);
    destroy = --file->f_refs == 0;
    spin_unlock(&open_files_lock);
    return destroy ? vfs_fd_destroy(file) : 0;
}

int vfs_fd_close(int fd)
{
    struct file *file;
    struct file **table;
    bool destroy;

    if (fd < 0 || fd >= CONFIG_TASK_MAX_FDS) return -EBADF;
    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    file = table[fd];
    if (file == NULL || file == VFS_FD_RESERVED) {
        spin_unlock(&open_files_lock);
        return -EBADF;
    }
    table[fd] = NULL;
    destroy = --file->f_refs == 0;
    spin_unlock(&open_files_lock);
    return destroy ? vfs_fd_destroy(file) : 0;
}

int vfs_fd_dup(int oldfd)
{
    struct file **table;
    struct file *file;
    int newfd;

    if (oldfd < 0 || oldfd >= CONFIG_TASK_MAX_FDS) return -EBADF;

    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    file = table[oldfd];
    if (file == NULL || file == VFS_FD_RESERVED) {
        spin_unlock(&open_files_lock);
        return -EBADF;
    }
    if (file->f_refs == UINT_MAX) {
        spin_unlock(&open_files_lock);
        return -EMFILE;
    }

    newfd = -1;
    for (int i = 0; i < CONFIG_TASK_MAX_FDS; i++) {
        if (table[i] == NULL) {
            newfd = i;
            break;
        }
    }
    if (newfd < 0) {
        spin_unlock(&open_files_lock);
        return -EMFILE;
    }

    file->f_refs++;
    table[newfd] = file;
    spin_unlock(&open_files_lock);
    return newfd;
}

int vfs_fd_dup2(int oldfd, int newfd)
{
    struct file **table;
    struct file *old_file;
    struct file *old_target = NULL;
    bool destroy_target = false;

    if (oldfd < 0 || oldfd >= CONFIG_TASK_MAX_FDS) return -EBADF;
    if (newfd < 0 || newfd >= CONFIG_TASK_MAX_FDS) return -EBADF;

    spin_lock(&open_files_lock);
    table = vfs_get_files_table();
    old_file = table[oldfd];
    if (old_file == NULL || old_file == VFS_FD_RESERVED) {
        spin_unlock(&open_files_lock);
        return -EBADF;
    }

    if (oldfd == newfd) {
        spin_unlock(&open_files_lock);
        return newfd;
    }

    if (old_file->f_refs == UINT_MAX) {
        spin_unlock(&open_files_lock);
        return -EMFILE;
    }

    old_target = table[newfd];
    if (old_target != NULL && old_target != VFS_FD_RESERVED) {
        destroy_target = --old_target->f_refs == 0;
    } else {
        old_target = NULL;
    }

    old_file->f_refs++;
    table[newfd] = old_file;
    spin_unlock(&open_files_lock);

    if (destroy_target && old_target != NULL) {
        vfs_fd_destroy(old_target);
    }
    return newfd;
}

void vfs_task_files_init(struct task_struct *task, struct task_struct *parent)
{
    int i;
    if (task == NULL) return;
    for (i = 0; i < CONFIG_TASK_MAX_FDS; i++) {
        task->files[i] = NULL;
    }
    if (parent == NULL) return;

    spin_lock(&open_files_lock);
    for (i = 0; i < CONFIG_TASK_MAX_FDS; i++) {
        struct file *f = parent->files[i];
        if (f != NULL && f != VFS_FD_RESERVED && f->f_refs != UINT_MAX) {
            f->f_refs++;
            task->files[i] = f;
        }
    }
    spin_unlock(&open_files_lock);
}

void vfs_task_files_cleanup(struct task_struct *task)
{
    int i;
    if (task == NULL) return;
    for (i = 0; i < CONFIG_TASK_MAX_FDS; i++) {
        struct file *f = NULL;
        bool destroy = false;

        spin_lock(&open_files_lock);
        f = task->files[i];
        if (f != NULL) {
            task->files[i] = NULL;
            if (f != VFS_FD_RESERVED) {
                destroy = --f->f_refs == 0;
            } else {
                f = NULL;
            }
        }
        spin_unlock(&open_files_lock);

        if (destroy && f != NULL) {
            vfs_fd_destroy(f);
        }
    }
}
