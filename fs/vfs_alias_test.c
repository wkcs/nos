/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <string.h>
#include "vfs_fd.h"

struct alias_readdir_check {
    int count;
    char first[FS_NAME_LEN];
};

static int alias_stop_readdir(void *context, const char *name, int len,
                              loff_t offset, u64 ino, unsigned type)
{
    struct alias_readdir_check *check = context;
    (void)offset;
    (void)ino;
    (void)type;
    if (check->count++ == 0 && len > 0 && len < FS_NAME_LEN) {
        memcpy(check->first, name, (size_t)len);
        check->first[len] = '\0';
    }
    return 1;
}

static int alias_accept_one(void *context, const char *name, int len,
                            loff_t offset, u64 ino, unsigned type)
{
    struct alias_readdir_check *check = context;
    int previous = check->count;
    (void)alias_stop_readdir(context, name, len, offset, ino, type);
    return previous != 0;
}

static int vfs_alias_test(void)
{
    struct dentry *root = vfs_get_root();
    struct dentry *original;
    int fd;

    fd = nos_open("/typecheck.tmp", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0) goto failed;
    original = vfs_lookup_path(root, "/typecheck.tmp");
    if (original == NULL) goto failed;
    {
        struct alias_readdir_check first = {0}, retry = {0}, accepted = {0}, next = {0};
        struct dentry *dir;
        struct file file = {0};
        if (nos_mkdir("/enum-test", 0755) != 0) goto failed;
        fd = nos_open("/enum-test/long filename.txt", O_CREAT | O_RDWR);
        if (fd < 0 || nos_close(fd) != 0) goto failed;
        fd = nos_open("/enum-test/b", O_CREAT | O_RDWR);
        if (fd < 0 || nos_close(fd) != 0) goto failed;
        dir = vfs_lookup_path(root, "/enum-test");
        if (dir == NULL) goto failed;
        file.f_inode = dir->d_inode;
        file.f_op = dir->d_inode->i_fop;
        file.f_path.dentry = dir;
        if (file.f_op->open(file.f_inode, &file) != 0 ||
            file.f_op->readdir(&file, &first, alias_stop_readdir) != 0 ||
            first.count != 1 || strcmp(first.first, "long filename.txt") != 0 || file.f_pos != 0 ||
            file.f_op->readdir(&file, &retry, alias_stop_readdir) != 0 ||
            retry.count != 1 || strcmp(first.first, retry.first) != 0 ||
            file.f_op->readdir(&file, &accepted, alias_accept_one) != 0 ||
            accepted.count != 2 || file.f_pos != 1 ||
            file.f_op->readdir(&file, &next, alias_stop_readdir) != 0 ||
            next.count != 1 || strcmp(first.first, next.first) == 0 ||
            file.f_op->release(file.f_inode, &file) != 0 ||
            nos_unlink("/enum-test/long filename.txt") != 0 || nos_unlink("/enum-test/b") != 0 ||
            nos_rmdir("/enum-test") != 0) goto failed;
    }
    {
        size_t used = mm_block_used_size();
        int i;
        for (i = 0; i < 16; i++) {
            if (vfs_lookup_path(root, "/TYPECHECK.TMP") != original ||
                vfs_lookup_path(root, "/TYPECH~1.TMP") != original) goto failed;
        }
        if (mm_block_used_size() != used) goto failed;
    }

    {
        struct file file = {0};
        struct dentry *entry;
        fd = nos_open("/direct.tmp", O_CREAT | O_RDWR);
        if (fd < 0 || nos_close(fd) != 0) goto failed;
        entry = vfs_lookup_path(root, "/direct.tmp");
        if (entry == NULL) goto failed;
        file.f_path.dentry = entry;
        file.f_inode = entry->d_inode;
        file.f_op = entry->d_inode->i_fop;
        if (file.f_op->open(file.f_inode, &file) != 0 || nos_unlink("/direct.tmp") != 0 ||
            file.f_inode->i_mode == 0 || file.f_op->release(file.f_inode, &file) != 0) goto failed;
    }
    {
        struct file *file;
        if (nos_mkdir("/pin-old", 0755) != 0 || nos_mkdir("/pin-new", 0755) != 0) goto failed;
        fd = nos_open("/pin-old/item", O_CREAT | O_RDWR);
        if (fd < 0) goto failed;
        file = vfs_fd_get(fd);
        if (file == NULL || nos_rename("/pin-old/item", "/pin-new/item") != 0 ||
            nos_rmdir("/pin-old") != 0 || file->f_path.dentry->d_parent == NULL ||
            strcmp((const char *)file->f_path.dentry->d_parent->d_name.name, "pin-new") != 0 ||
            nos_close(fd) != 0 || vfs_fd_put(file) != 0 ||
            nos_unlink("/pin-new/item") != 0 || nos_rmdir("/pin-new") != 0) goto failed;
    }
    if (nos_unlink("/TYPECH~1.TMP") != 0 ||
        vfs_lookup_path(root, "/typecheck.tmp") != NULL || nos_rmdir("/SYS") != -EBUSY) goto failed;

    {
        size_t used = mm_block_used_size();
        int i;
        for (i = 0; i < 20; i++) {
            fd = nos_open("/reclaim.tmp", O_CREAT | O_RDWR);
            if (fd < 0 || nos_close(fd) != 0 || nos_unlink("/reclaim.tmp") != 0) goto failed;
        }
        if (mm_block_used_size() != used) goto failed;
    }

    if (nos_mkdir("/AliasDir", 0755) != 0 ||
        vfs_lookup_path(root, "/ALIASDIR") != vfs_lookup_path(root, "/AliasDir") ||
        nos_rmdir("/ALIASDIR") != 0 || vfs_lookup_path(root, "/AliasDir") != NULL) goto failed;

    fd = nos_open("/longfilename.txt", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 ||
        vfs_lookup_path(root, "/LONGFI~1.TXT") == NULL ||
        nos_rename("/LONGFI~1.TXT", "/renamed.txt") != 0 ||
        vfs_lookup_path(root, "/longfilename.txt") != NULL ||
        vfs_lookup_path(root, "/renamed.txt") == NULL) goto failed;
    fd = nos_open("/newfile.txt", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 ||
        vfs_lookup_path(root, "/NEWFILE.TXT") != vfs_lookup_path(root, "/newfile.txt") ||
        vfs_lookup_path(root, "/NEWFILE.TXT") == vfs_lookup_path(root, "/renamed.txt") ||
        nos_unlink("/newfile.txt") != 0 || nos_unlink("/renamed.txt") != 0) goto failed;
    {
        struct dentry *mountpoint = NULL;
        struct file *file;
        unsigned int root_refs = root->d_refs;
        if (vfs_make_dir(root, "pin-test", 0755, &mountpoint) != 0) goto failed;
        fd = nos_open("/pin-test/item", O_CREAT | O_RDWR);
        if (fd < 0 || nos_write(fd, "X", 1) != 1 || nos_close(fd) != 0) goto failed;
        fd = nos_open("/pin-test/item", O_RDONLY);
        if (fd < 0) goto failed;
        file = vfs_fd_get(fd);
        if (file == NULL || file->f_path.dentry == NULL || file->f_path.dentry->d_refs < 2) goto failed;
        if (nos_unlink("/pin-test/item") != 0 || file->f_path.dentry->d_refs == 0 ||
            strcmp((const char *)file->f_path.dentry->d_name.name, "item") != 0 ||
            nos_rmdir("/pin-test") != 0 || file->f_path.dentry->d_parent == NULL ||
            strcmp((const char *)file->f_path.dentry->d_parent->d_name.name, "pin-test") != 0 ||
            nos_close(fd) != 0 || vfs_fd_put(file) != 0 || root->d_refs != root_refs) goto failed;
    }
    pr_info("vfs fat alias OK\n");
    return 0;
failed:
    pr_err("vfs fat alias FAILED\n");
    return -EIO;
}
SYS_INIT(vfs_alias_test, INIT_LEVEL_APPLICATION);
