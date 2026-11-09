#ifndef TEST_SPINLOCK_H
#define TEST_SPINLOCK_H
#include <pthread.h>
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define SPIN_LOCK_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define spin_lock_init(l) pthread_mutex_init(l, NULL)
#define spin_lock(l) pthread_mutex_lock(l)
#define spin_unlock(l) pthread_mutex_unlock(l)
#endif
