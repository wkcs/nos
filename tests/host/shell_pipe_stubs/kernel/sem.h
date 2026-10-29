/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef TEST_SHELL_PIPE_SEM_H
#define TEST_SHELL_PIPE_SEM_H
#include <pthread.h>
#include <stdint.h>
typedef struct { pthread_mutex_t lock; pthread_cond_t ready; unsigned int count; } sem_t;
static inline int sem_init(sem_t *s, uint8_t count) { pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->ready, NULL); s->count = count; return 0; }
static inline void sem_get(sem_t *s) { pthread_mutex_lock(&s->lock); while (!s->count) pthread_cond_wait(&s->ready, &s->lock); s->count--; pthread_mutex_unlock(&s->lock); }
static inline void sem_send_one(sem_t *s) { pthread_mutex_lock(&s->lock); s->count++; pthread_cond_signal(&s->ready); pthread_mutex_unlock(&s->lock); }
#endif
