#ifndef TEST_SHELL_PIPE_NOS_SEM_H
#define TEST_SHELL_PIPE_NOS_SEM_H
#include <pthread.h>
#include <stdint.h>
#include <nos/timeout.h>
struct nos_sem {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    unsigned int count;
    unsigned int limit;
};
static inline int nos_sem_init(struct nos_sem *s, uint32_t initial, uint32_t limit)
{
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->ready, NULL);
    s->count = initial;
    s->limit = limit;
    return 0;
}
static inline int nos_sem_take(struct nos_sem *s, nos_timeout_t timeout)
{
    (void)timeout;
    pthread_mutex_lock(&s->lock);
    while (!s->count) {
        pthread_cond_wait(&s->ready, &s->lock);
    }
    s->count--;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static inline int nos_sem_give(struct nos_sem *s)
{
    pthread_mutex_lock(&s->lock);
    if (s->count < s->limit) {
        s->count++;
        pthread_cond_signal(&s->ready);
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}
#endif
