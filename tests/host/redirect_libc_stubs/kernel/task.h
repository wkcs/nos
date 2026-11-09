#ifndef TEST_TASK_H
#define TEST_TASK_H
#include <kernel/spinlock.h>
#include <stdint.h>
#define CONFIG_TASK_MAX_FDS 16
struct file;
struct task_struct {
    uint32_t pid;
    struct file *files[CONFIG_TASK_MAX_FDS];
    spinlock_t files_lock;
};
#ifndef TEST_CUSTOM_TASK_CURRENT
static inline struct task_struct *task_current(void) { return NULL; }
#else
struct task_struct *task_current(void);
#endif
#endif
