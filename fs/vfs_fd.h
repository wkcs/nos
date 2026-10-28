/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_VFS_FD_H
#define NOS_VFS_FD_H

struct file;
int vfs_fd_reserve(void);
void vfs_fd_cancel(int fd);
void vfs_fd_publish(int fd, struct file *file);
struct file *vfs_fd_get(int fd);
int vfs_fd_put(struct file *file);
int vfs_fd_close(int fd);

#endif
