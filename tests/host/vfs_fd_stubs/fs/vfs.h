#ifndef TEST_VFS_FD_VFS_H
#define TEST_VFS_FD_VFS_H
struct inode { int unused; };
struct file;
struct file_operations { int (*release)(struct inode *, struct file *); };
struct file {
    struct inode *f_inode;
    const struct file_operations *f_op;
    unsigned int f_refs;
    int marker;
};
#endif
