/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/mm.h>

static unsigned int allocations;
void *kmalloc(size_t size, gfp_t flags)
{
    void *ptr = malloc(size);
    (void)flags;
    if (ptr != NULL) allocations++;
    return ptr;
}
void *kzalloc(size_t size, gfp_t flags)
{
    void *ptr = kmalloc(size, flags);
    if (ptr != NULL) memset(ptr, 0, size);
    return ptr;
}
char *kstrdup(const char *str, gfp_t flags)
{
    size_t length = strlen(str) + 1U;
    char *copy = kmalloc(length, flags);
    if (copy != NULL) memcpy(copy, str, length);
    return copy;
}
void kfree(void *ptr)
{
    if (ptr != NULL) {
        assert(allocations != 0U);
        allocations--;
        free(ptr);
    }
}

#include "../../fs/vfs.c"

static void test_nested_mount_lifetime(void)
{
    struct inode dir_inode = {0}, mounted_inode = {0};
    struct dentry root = {.d_inode = &dir_inode};
    struct dentry volume = {.d_inode = &mounted_inode};
    struct dentry *parent, *child;
    struct vfs_mount_info info;

    root_dentry = &root;
    parent = vfs_alloc_dentry(&root, "mnt", 3U);
    assert(parent != NULL);
    parent->d_inode = &dir_inode;
    assert(vfs_bind_mount(parent, &volume) == 0);
    child = vfs_alloc_dentry(parent, "sub", 3U);
    assert(child != NULL);
    child->d_inode = &dir_inode;
    list_add_tail(&child->d_child, &parent->d_subdirs);
    assert(vfs_bind_mount(child, &volume) == 0);

    assert(vfs_bind_mount(parent, &volume) == -EBUSY);
    assert(vfs_unmount(parent) == -EBUSY);
    assert(vfs_get_mount_info(2, &info) == 0);
    assert(strcmp(info.mount_path, "/mnt/sub") == 0);

    assert(vfs_unmount(child) == 0);
    assert(vfs_unmount(parent) == 0);
    kfree(parent);
    root_dentry = NULL;
    assert(allocations == 0U);
}

int main(void)
{
    struct file_system_type type = {.name = "littlefs"};
    struct super_block sb = {.s_type = &type};
    unsigned int i;

    /* Replacing active.nkc repeatedly must not retain one inode per save. */
    for (i = 0U; i < 1000U; i++) {
        struct dentry *dentry = vfs_alloc_dentry(NULL, "active.nkc", 10U);
        assert(dentry != NULL);
        dentry->d_sb = &sb;
        dentry->d_inode = nos_vfs_alloc_inode(&sb);
        assert(dentry->d_inode != NULL);
        vfs_drop_removed_dentry(dentry);
        assert(allocations == 0U);
    }

    /* An unlinked open file retains its inode until its last pin is put. */
    {
        struct dentry *dentry = vfs_alloc_dentry(NULL, "active.nkc", 10U);
        dentry->d_sb = &sb;
        dentry->d_inode = nos_vfs_alloc_inode(&sb);
        assert(vfs_dentry_get(dentry) == 0);
        vfs_drop_removed_dentry(dentry);
        assert(allocations == 2U);
        vfs_dentry_put(dentry);
        assert(allocations == 0U);
    }
    test_nested_mount_lifetime();
    return 0;
}
