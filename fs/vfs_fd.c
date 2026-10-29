/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <limits.h>
#include "vfs_fd.h"

#define VFS_MAX_OPEN_FILES 16

static struct file *open_files[VFS_MAX_OPEN_FILES];
static bool open_file_slots[VFS_MAX_OPEN_FILES];
static SPINLOCK(open_files_lock);

static int vfs_fd_destroy(struct file *file)
{
    int rc = 0;
    if (file->f_op != NULL && file->f_op->release != NULL)
        rc = file->f_op->release(file->f_inode, file);
    kfree(file);
    return rc;
}

int vfs_fd_reserve(void)
{
    int fd;
    spin_lock(&open_files_lock);
    for (fd = 0; fd < VFS_MAX_OPEN_FILES; fd++) {
        if (!open_file_slots[fd]) {
            open_file_slots[fd] = true;
            spin_unlock(&open_files_lock);
            return fd;
        }
    }
    spin_unlock(&open_files_lock);
    return -EMFILE;
}

void vfs_fd_cancel(int fd)
{
    spin_lock(&open_files_lock);
    open_file_slots[fd] = false;
    spin_unlock(&open_files_lock);
}

void vfs_fd_publish(int fd, struct file *file)
{
    sem_init(&file->f_pos_sem, 1);
    spin_lock(&open_files_lock);
    file->f_refs = 1;
    open_files[fd] = file;
    spin_unlock(&open_files_lock);
}

struct file *vfs_fd_get(int fd)
{
    struct file *file = NULL;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return NULL;
    spin_lock(&open_files_lock);
    if (open_files[fd] != NULL && open_files[fd]->f_refs != UINT_MAX) {
        file = open_files[fd];
        file->f_refs++;
    }
    spin_unlock(&open_files_lock);
    return file;
}

int vfs_fd_put(struct file *file)
{
    bool destroy;
    spin_lock(&open_files_lock);
    destroy = --file->f_refs == 0;
    spin_unlock(&open_files_lock);
    return destroy ? vfs_fd_destroy(file) : 0;
}

int vfs_fd_close(int fd)
{
    struct file *file;
    bool destroy;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return -EBADF;
    spin_lock(&open_files_lock);
    file = open_files[fd];
    if (file == NULL) {
        spin_unlock(&open_files_lock);
        return -EBADF;
    }
    open_files[fd] = NULL;
    open_file_slots[fd] = false;
    destroy = --file->f_refs == 0;
    spin_unlock(&open_files_lock);
    return destroy ? vfs_fd_destroy(file) : 0;
}
