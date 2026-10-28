#ifndef RAMFS_TEST_VFS_H
#define RAMFS_TEST_VFS_H
#include <fs/fs.h>
#include <kernel/list.h>
#include <pthread.h>
struct mutex { pthread_mutex_t native; };
static inline void mutex_init(struct mutex *m) { pthread_mutex_init(&m->native, NULL); }
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->native); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->native); }
struct inode; struct dentry; struct file; struct super_block; struct file_system_type;
typedef int (*filldir_t)(void *, const char *, int, off_t, uint64_t, unsigned);
struct file_operations {
    ssize_t (*read)(struct file *, char *, size_t, off_t *);
    ssize_t (*write)(struct file *, const char *, size_t, off_t *);
    int (*readdir)(struct file *, void *, filldir_t);
};
struct inode_operations {
    int (*create)(struct inode *, struct dentry *, int);
    int (*mkdir)(struct inode *, struct dentry *, int);
    struct dentry *(*lookup)(struct inode *, struct dentry *);
};
struct inode {
    int i_mode; size_t i_size; uint64_t i_ino;
    struct timespec i_atime, i_mtime, i_ctime;
    struct super_block *i_sb; void *i_private; struct mutex i_mutex;
    const struct inode_operations *i_op; const struct file_operations *i_fop;
};
struct dentry { struct inode *d_inode; struct super_block *d_sb; struct { const char *name; } d_name; };
struct file { struct inode *f_inode; off_t f_pos; };
struct super_operations { int (*statfs)(void *, void *); };
struct super_block { unsigned long s_blocksize; unsigned char s_blocksize_bits; const struct super_operations *s_op; struct dentry *s_root; };
struct file_system_type { const char *name; struct dentry *(*mount)(struct file_system_type *, int, const char *, void *); void (*kill_sb)(void *); };
struct inode *nos_vfs_alloc_inode(struct super_block *sb);
void nos_vfs_destroy_inode(struct inode *inode);
int register_filesystem(struct file_system_type *fs);
#endif
