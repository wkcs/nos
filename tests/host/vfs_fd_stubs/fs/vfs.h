#ifndef TEST_VFS_FD_VFS_H
#define TEST_VFS_FD_VFS_H
#include <fs/fs.h>
#include <kernel/sem.h>
struct inode { loff_t i_size; };
struct file;
struct file_operations {
    int (*release)(struct inode *, struct file *);
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
    loff_t (*llseek)(struct file *, loff_t, int);
};
struct file {
    struct inode *f_inode;
    const struct file_operations *f_op;
    unsigned int f_refs;
    unsigned int f_flags;
    loff_t f_pos;
    sem_t f_pos_sem;
    int marker;
};
#endif
