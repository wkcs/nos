/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/mm.h>
#include <kernel/errno.h>
void *kmalloc(size_t n, gfp_t f) { (void)f; return malloc(n); }
void *kzalloc(size_t n, gfp_t f) { (void)f; return calloc(1,n); }
void kfree(void *p) { free(p); }
char *kstrdup(const char *s, gfp_t f) { (void)f; return strdup(s); }
#include "../../fs/vfs.c"
static struct inode child;
static int creates;
static struct file *opened;
int vfs_fd_reserve(void) { return 3; }
void vfs_fd_cancel(int fd) { (void)fd; }
void vfs_fd_publish(int fd, struct file *file) { assert(fd == 3); opened = file; }
static int backend_open(struct inode *inode, struct file *file)
{
    (void)inode;
    assert(!(file->f_flags & (O_CREAT | O_EXCL)));
    return 0;
}
static const struct file_operations file_ops = {.open = backend_open};
static int create(struct inode *dir, struct dentry *entry, int mode)
{
    (void)dir;
    creates++;
    child.i_mode = mode;
    child.i_fop = &file_ops;
    entry->d_inode = &child;
    return 0;
}
static const struct inode_operations ops = {.create = create};
int main(void)
{
    struct inode dir = {.i_mode=S_IFDIR|0755,.i_op=&ops};
    struct dentry root = {.d_inode=&dir,.d_refs=1}, *entry = NULL;
    bool created;
    root.d_parent = &root; root_dentry = &root;
    assert(vfs_resolve_open_path("/object", O_WRONLY|O_CREAT|O_EXCL, &entry, &created) == 0);
    assert(created && entry && creates == 1);
    assert(vfs_resolve_open_path("/object", O_WRONLY|O_CREAT|O_EXCL, &entry, &created) == -EEXIST);
    assert(!created && creates == 1);
    assert(vfs_resolve_open_path("/object", O_WRONLY|O_CREAT, &entry, &created) == 0 && !created);
    assert(vfs_resolve_open_path("/absent", O_RDONLY, &entry, &created) == -ENOENT);
    assert(nos_open("/through-open", O_CREAT|O_EXCL|O_WRONLY, 0600) == 3);
    assert(opened != NULL && creates == 2);
    struct file *saved = opened; opened = NULL;
    assert(nos_open("/through-open", O_CREAT|O_EXCL|O_WRONLY, 0600) == -EEXIST);
    assert(opened == NULL && creates == 2);
    vfs_dentry_put(saved->f_path.dentry); kfree(saved);
    return 0;
}
