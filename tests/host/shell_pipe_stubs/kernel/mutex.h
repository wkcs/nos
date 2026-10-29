/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef TEST_SHELL_PIPE_MUTEX_H
#define TEST_SHELL_PIPE_MUTEX_H
#include <pthread.h>
struct mutex { pthread_mutex_t native; };
static inline int mutex_init(struct mutex *m) { return pthread_mutex_init(&m->native, NULL); }
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->native); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->native); }
#endif
