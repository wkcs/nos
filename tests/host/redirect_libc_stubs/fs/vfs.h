#ifndef TEST_VFS_H
#define TEST_VFS_H
#include <fs/fs.h>
#include <kernel/mutex.h>
#include <sys/stat.h>
struct inode { loff_t i_size; mode_t i_mode; };
struct file;
struct super_block;
struct dentry;
struct file_operations {
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
    int (*fsync)(struct file *, loff_t, loff_t, int);
    loff_t (*llseek)(struct file *, loff_t, int);
};
struct super_operations {
    int (*sync_fs)(struct super_block *sb, int wait);
};
struct super_block {
    const struct super_operations *s_op;
};
struct dentry {
    struct super_block *d_sb;
};
struct file {
    struct { struct dentry *dentry; } f_path;
    struct inode *f_inode;
    const struct file_operations *f_op;
    unsigned int f_refs;
    unsigned int f_flags;
    loff_t f_pos;
    struct mutex f_pos_lock;
    void *private_data;
    int marker;
};
struct file *vfs_fd_get(int fd);
int vfs_fd_put(struct file *file);
struct vfs_mount_entry {
    const char *dev_name;
    const char *mount_path;
    const char *fs_name;
    struct dentry *dentry;
};
int vfs_get_mount_count(void);
int vfs_get_mount_entry(int index, struct vfs_mount_entry *entry);
static inline void vfs_dentry_put(void *dentry) { (void)dentry; }
#endif
