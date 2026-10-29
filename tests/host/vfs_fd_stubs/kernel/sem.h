#ifndef TEST_VFS_FD_SEM_H
#define TEST_VFS_FD_SEM_H
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t sem_t;
static inline int sem_init(sem_t *sem, unsigned int value)
{ (void)value; return pthread_mutex_init(sem, NULL); }
static inline int sem_get_timeout(sem_t *sem, uint32_t tick)
{ (void)tick; return pthread_mutex_lock(sem); }
static inline void sem_send_one(sem_t *sem) { pthread_mutex_unlock(sem); }
#endif
