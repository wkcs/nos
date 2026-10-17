/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Thread_local unsigned initialized_tls = 0x7152;
static _Thread_local unsigned zero_tls;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t key;
static unsigned ready, go, count, cpu_mask, destructors, detach_go, cleanups;
static FILE *shared_stream;
static char stream_buffer[256];

static int fail(const char *name, int error)
{
    fprintf(stderr, "FAIL %s (%d)\n", name, error);
    return 90;
}
static void destructor(void *value)
{
    if (value == (void *)1) __atomic_add_fetch(&destructors, 1, __ATOMIC_RELEASE);
}
static void *worker(void *argument)
{
    unsigned identity = (unsigned)(uintptr_t)argument;
    if (initialized_tls != 0x7152 || zero_tls || pthread_setspecific(key, (void *)1)) return (void *)0x1001;
    initialized_tls += identity; zero_tls = identity;
    if (pthread_mutex_lock(&lock)) return (void *)0x1002;
    ready++; pthread_cond_broadcast(&condition);
    while (!go) if (pthread_cond_wait(&condition, &lock)) return (void *)0x1003;
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 64; i++) {
        unsigned char *memory = malloc(256);
        if (!memory) return (void *)0x1004;
        memset(memory, (int)(identity + i), 256);
        int cpu = sched_getcpu();
        if (cpu < 0 || cpu >= 32) { free(memory); return (void *)0x1005; }
        __atomic_fetch_or(&cpu_mask, 1u << cpu, __ATOMIC_RELAXED);
        if (pthread_mutex_lock(&lock)) { free(memory); return (void *)0x1006; }
        count++;
        pthread_mutex_unlock(&lock);
        if (fputc('A' + (int)identity - 1, shared_stream) == EOF) { free(memory); return (void *)0x1007; }
        /* Successful malloc/stdio calls may change errno. Isolate the TLS
         * assertion from those calls and exercise only the yielding window. */
        errno = 100 + (int)identity;
        sched_yield();
        for (unsigned j = 0; j < 256; j++)
            if (memory[j] != (unsigned char)(identity + i)) { free(memory); return (void *)0x1008; }
        if (errno != 100 + (int)identity || initialized_tls != 0x7152 + identity || zero_tls != identity) {
            free(memory); return (void *)0x1009;
        }
        free(memory);
    }
    return argument;
}
static void *detached_worker(void *argument)
{
    unsigned identity = (unsigned)(uintptr_t)argument;
    if (pthread_setspecific(key, (void *)1)) return (void *)-1;
    while (__atomic_load_n(&detach_go, __ATOMIC_ACQUIRE) < identity) sched_yield();
    return NULL;
}
static void cancellation_cleanup(void *argument)
{
    if (argument) pthread_mutex_unlock(&lock);
    __atomic_add_fetch(&cleanups, 1, __ATOMIC_RELEASE);
}
static void *cancel_waiter(void *unused)
{
    (void)unused;
    if (pthread_setspecific(key, (void *)1) || pthread_mutex_lock(&lock)) return (void *)-1;
    pthread_cleanup_push(cancellation_cleanup, (void *)1);
    ready = 1; pthread_cond_broadcast(&condition);
    for (;;) pthread_cond_wait(&condition, &lock);
    pthread_cleanup_pop(1);
    return NULL;
}
static void *cancel_async(void *unused)
{
    (void)unused;
    if (pthread_setspecific(key, (void *)1)) return (void *)-1;
    pthread_cleanup_push(cancellation_cleanup, NULL);
    if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL)) return (void *)-1;
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile("" ::: "memory");
    pthread_cleanup_pop(1);
    return NULL;
}
static pthread_mutex_t robust;
static void *robust_owner(void *unused)
{ (void)unused; return (void *)(uintptr_t)pthread_mutex_lock(&robust); }

int main(void)
{
    pthread_t threads[3];
    if (pthread_key_create(&key, destructor)) return fail("glibc_pthread_key", errno);
    shared_stream = fopen("glibc-threads", "w+");
    if (!shared_stream || setvbuf(shared_stream, stream_buffer, _IOFBF, sizeof(stream_buffer)))
        return fail("glibc_pthread_stream", errno);
    for (unsigned i = 0; i < 3; i++) {
        int rc = pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1));
        if (rc) return fail("glibc_pthread_default_create", rc);
    }
    puts("PASS glibc_pthread_default_create");
    pthread_mutex_lock(&lock);
    while (ready != 3) pthread_cond_wait(&condition, &lock);
    go = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 3; i++) {
        void *result;
        int rc = pthread_join(threads[i], &result);
        if (rc || result != (void *)(uintptr_t)(i + 1))
            return fail("glibc_pthread_join", rc ? rc : (int)(uintptr_t)result);
    }
    if (count != 192 || destructors != 3 || initialized_tls != 0x7152 || zero_tls)
        return fail("glibc_pthread_tls_errno_malloc", 1);
    if (fflush(shared_stream) || fseek(shared_stream, 0, SEEK_SET)) return fail("glibc_pthread_stdio", errno);
    unsigned counts[3] = { 0 };
    for (unsigned i = 0; i < 192; i++) {
        int c = fgetc(shared_stream);
        if (c < 'A' || c > 'C') return fail("glibc_pthread_stdio", c);
        counts[c - 'A']++;
    }
    if (counts[0] != 64 || counts[1] != 64 || counts[2] != 64) return fail("glibc_pthread_stdio_counts", 1);
    if (fclose(shared_stream)) return fail("glibc_pthread_stdio_close", errno);
    if (unlink("glibc-threads")) return fail("glibc_pthread_stdio_unlink", errno);
    puts("PASS glibc_cond_broadcast_tls_errno_malloc_stdio");
    printf("glibc_pthread_cpu_mask=%x\n", cpu_mask);
    const char *expected_cpus = getenv("NOS_EXPECT_CPUS");
    if (expected_cpus && atoi(expected_cpus) > 1 && !(cpu_mask & (cpu_mask - 1)))
        return fail("glibc_pthread_cross_cpu_execution", (int)cpu_mask);

    for (unsigned i = 1; i <= 8; i++) {
        pthread_t detached;
        int rc = pthread_create(&detached, NULL, detached_worker, (void *)(uintptr_t)i);
        if (rc || (rc = pthread_detach(detached))) return fail("glibc_pthread_detach", rc);
        __atomic_store_n(&detach_go, i, __ATOMIC_RELEASE);
        struct timespec pause = { 0, 10000000 };
        unsigned attempts = 0;
        while (__atomic_load_n(&destructors, __ATOMIC_ACQUIRE) < 3 + i && attempts++ < 200) nanosleep(&pause, NULL);
        if (destructors != 3 + i) return fail("glibc_pthread_detach_tsd", 1);
        nanosleep(&pause, NULL);
    }
    puts("PASS glibc_pthread_detach_tsd_exit");
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) || pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) ||
        pthread_mutex_init(&robust, &attributes)) return fail("glibc_robust_init", 1);
    pthread_mutexattr_destroy(&attributes);
    pthread_t owner;
    void *result;
    int rc = pthread_create(&owner, NULL, robust_owner, NULL);
    if (rc || (rc = pthread_join(owner, &result)) || result) return fail("glibc_robust_owner", rc);
    rc = pthread_mutex_lock(&robust);
    if (rc != EOWNERDEAD || pthread_mutex_consistent(&robust) || pthread_mutex_unlock(&robust) ||
        pthread_mutex_destroy(&robust)) return fail("glibc_robust_owner_death", rc);
    puts("PASS glibc_pthread_robust_owner_death");

    ready = 0;
    pthread_t cancelled;
    rc = pthread_create(&cancelled, NULL, cancel_waiter, NULL);
    if (rc) return fail("glibc_cancel_create", rc);
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    if ((rc = pthread_cancel(cancelled)) || (rc = pthread_join(cancelled, &result)) || result != PTHREAD_CANCELED ||
        cleanups != 1 || destructors != 12) return fail("glibc_cancel_condition_cleanup", rc);
    if (pthread_mutex_trylock(&lock) || pthread_mutex_unlock(&lock)) return fail("glibc_cancel_mutex_reacquire", 1);
    puts("PASS glibc_pthread_cancel_condition_cleanup");
    __atomic_store_n(&ready, 0, __ATOMIC_RELEASE);
    rc = pthread_create(&cancelled, NULL, cancel_async, NULL);
    if (rc) return fail("glibc_cancel_async_create", rc);
    while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) sched_yield();
    if ((rc = pthread_cancel(cancelled)) || (rc = pthread_join(cancelled, &result)) || result != PTHREAD_CANCELED ||
        cleanups != 2 || destructors != 13) return fail("glibc_cancel_async_cleanup", rc);
    puts("PASS glibc_pthread_cancel_async_cleanup");
    if (pthread_key_delete(key) || pthread_cond_destroy(&condition) || pthread_mutex_destroy(&lock))
        return fail("glibc_pthread_final_destroy", 1);
    return 0;
}
