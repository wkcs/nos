/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_VFS_FD_H
#define NOS_VFS_FD_H

struct file;
struct task_struct;

int vfs_fd_reserve(void);
void vfs_fd_cancel(int fd);
void vfs_fd_publish(int fd, struct file *file);
struct file *vfs_fd_get(int fd);
int vfs_fd_put(struct file *file);
int vfs_fd_close(int fd);
int vfs_fd_dup(int oldfd);
int vfs_fd_dup2(int oldfd, int newfd);
void vfs_task_files_init(struct task_struct *task, struct task_struct *parent);
void vfs_task_files_cleanup(struct task_struct *task);

#endif
