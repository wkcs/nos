/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/mm.h>

static unsigned int allocations;
char *kstrdup(const char *str, gfp_t flags);
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
void kfree(void *ptr)
{
    if (ptr != NULL) {
        assert(allocations != 0U);
        allocations--;
        free(ptr);
    }
}

#include "../../fs/vfs.c"

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
    return 0;
}
