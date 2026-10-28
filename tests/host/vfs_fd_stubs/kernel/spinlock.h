#ifndef TEST_VFS_FD_SPINLOCK_H
#define TEST_VFS_FD_SPINLOCK_H
#include <pthread.h>
#define SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define spin_lock(lock) pthread_mutex_lock(lock)
#define spin_unlock(lock) pthread_mutex_unlock(lock)
#endif
