#ifndef TEST_VFS_FD_TASK_H
#define TEST_VFS_FD_TASK_H

#include <kernel/kernel.h>

#ifndef CONFIG_TASK_MAX_FDS
#define CONFIG_TASK_MAX_FDS 8
#endif

struct file;

struct task_struct {
    struct file *files[CONFIG_TASK_MAX_FDS];
};

void vfs_task_files_init(struct task_struct *task, struct task_struct *parent);
void vfs_task_files_cleanup(struct task_struct *task);

#ifdef TEST_CUSTOM_TASK_CURRENT
struct task_struct *task_current(void);
#else
static inline struct task_struct *task_current(void)
{
    return NULL;
}
#endif

#endif
