/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t barrier;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static unsigned counter, once_calls, destructor_calls;
struct destructor_value { unsigned iteration; };

static int failed(const char *name, int error)
{ fprintf(stderr, "FAIL %s (%d)\n", name, error); return 92; }
static void initialize_once(void)
{
    struct timespec pause = { 0, 10000000 };
    nanosleep(&pause, NULL);
    once_calls++;
}
static void destructor(void *pointer)
{
    struct destructor_value *value = pointer;
    __atomic_add_fetch(&destructor_calls, 1, __ATOMIC_RELAXED);
    if (++value->iteration < 3) {
        if (pthread_setspecific(key, value)) abort();
    } else free(value);
}
static int barrier_wait(void)
{
    int rc = pthread_barrier_wait(&barrier);
    return rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD ? 0 : rc;
}
static void *worker(void *argument)
{
    struct destructor_value *value = calloc(1, sizeof(*value));
    if (!value) return (void *)0x2001;
    if (pthread_setspecific(key, value)) { free(value); return (void *)0x2002; }
    if (barrier_wait() || pthread_once(&once, initialize_once)) return (void *)0x2003;
    for (unsigned i = 0; i < 128; i++) {
        if (pthread_rwlock_rdlock(&rwlock)) return (void *)0x2004;
        unsigned observed = counter;
        if (pthread_rwlock_unlock(&rwlock) || observed > 384) return (void *)0x2005;
        if (pthread_rwlock_wrlock(&rwlock)) return (void *)0x2006;
        counter++;
        if (pthread_rwlock_unlock(&rwlock)) return (void *)0x2007;
    }
    if (barrier_wait()) return (void *)0x2008;
    return argument;
}
static sem_t semaphore;
static unsigned producer_go;
static void *producer(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&producer_go, __ATOMIC_ACQUIRE)) {
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }
    struct timespec pause = { 0, 20000000 };
    if (nanosleep(&pause, NULL) || sem_post(&semaphore)) return (void *)-1;
    return (void *)1;
}
static int64_t elapsed_ns(const struct timespec *before, const struct timespec *after)
{ return (int64_t)(after->tv_sec - before->tv_sec) * 1000000000 + after->tv_nsec - before->tv_nsec; }
int main(void)
{
    if (pthread_key_create(&key, destructor) || pthread_barrier_init(&barrier, NULL, 4))
        return failed("glibc_sync_setup", 1);
    pthread_t threads[3];
    for (unsigned i = 0; i < 3; i++) {
        int rc = pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1));
        if (rc) return failed("glibc_sync_create", rc);
    }
    if (barrier_wait() || barrier_wait()) return failed("glibc_sync_barrier", 1);
    for (unsigned i = 0; i < 3; i++) {
        void *result;
        int rc = pthread_join(threads[i], &result);
        if (rc || result != (void *)(uintptr_t)(i + 1))
            return failed("glibc_sync_join", rc ? rc : (int)(uintptr_t)result);
    }
    if (counter != 384 || once_calls != 1 || pthread_rwlock_trywrlock(&rwlock) ||
        pthread_rwlock_unlock(&rwlock) || pthread_rwlock_tryrdlock(&rwlock) || pthread_rwlock_unlock(&rwlock))
        return failed("glibc_rwlock_barrier_once", 1);
    puts("PASS glibc_rwlock_barrier_once");
    if (destructor_calls != 9 || pthread_getspecific(key) || pthread_key_delete(key))
        return failed("glibc_tsd_destructor_reentry", (int)destructor_calls);
    puts("PASS glibc_tsd_destructor_reentry");
    if (pthread_barrier_destroy(&barrier) || pthread_rwlock_destroy(&rwlock))
        return failed("glibc_sync_destroy", 1);
    if (sem_init(&semaphore, 0, 0)) return failed("glibc_semaphore_init", errno);
    errno = 0;
    if (sem_trywait(&semaphore) != -1 || errno != EAGAIN) return failed("glibc_semaphore_empty", errno);
    pthread_t posting;
    int rc = pthread_create(&posting, NULL, producer, NULL);
    if (rc) return failed("glibc_semaphore_create", rc);
    struct timespec before, after;
    if (clock_gettime(CLOCK_MONOTONIC, &before)) return failed("glibc_semaphore_clock", errno);
    __atomic_store_n(&producer_go, 1, __ATOMIC_RELEASE);
    if (sem_wait(&semaphore) || clock_gettime(CLOCK_MONOTONIC, &after) || elapsed_ns(&before, &after) < 10000000)
        return failed("glibc_semaphore_wait_post", errno);
    void *result;
    if ((rc = pthread_join(posting, &result)) || result != (void *)1)
        return failed("glibc_semaphore_producer_join", rc);
    int value = -1;
    if (sem_getvalue(&semaphore, &value) || value || sem_post(&semaphore) ||
        sem_getvalue(&semaphore, &value) || value != 1 || sem_trywait(&semaphore))
        return failed("glibc_semaphore_count", errno);
    puts("PASS glibc_unnamed_semaphore_wait_post");
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) || clock_gettime(CLOCK_MONOTONIC, &before))
        return failed("glibc_semaphore_realtime", errno);
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    errno = 0;
    if (sem_timedwait(&semaphore, &deadline) != -1 || errno != ETIMEDOUT)
        return failed("glibc_semaphore_timedwait", errno);
    if (clock_gettime(CLOCK_MONOTONIC, &after) || elapsed_ns(&before, &after) < 10000000 || sem_destroy(&semaphore))
        return failed("glibc_semaphore_timeout_duration", errno);
    puts("PASS glibc_unnamed_semaphore_timedwait");
    return 0;
}
