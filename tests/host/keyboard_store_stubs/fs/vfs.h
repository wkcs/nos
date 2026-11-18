/** Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#ifndef TEST_STORE_VFS_H
#define TEST_STORE_VFS_H
#include <fs/fs.h>
#include <stdint.h>
typedef uint64_t u64;
struct file;
struct inode;
typedef int (*filldir_t)(void *, const char *, int, loff_t, u64, unsigned);
struct file_operations {
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    int (*readdir)(struct file *, void *, filldir_t);
};
struct inode { unsigned int i_mode; const struct file_operations *i_fop; };
struct dentry { struct inode *d_inode; char path[512]; };
struct file {
    struct inode *f_inode;
    const struct file_operations *f_op;
    struct { struct dentry *dentry; } f_path;
    void *private_data;
};
struct dentry *vfs_get_root(void);
struct dentry *vfs_lookup_path(struct dentry *, const char *);
#endif
