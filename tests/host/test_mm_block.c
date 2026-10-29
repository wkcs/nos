/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mm.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define THREADS 8
#define ROUNDS 3000
struct live { char *ptr; size_t size; };
static struct live live[THREADS];
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
extern void *__kalloc(u32, gfp_t, pid_t);
extern int __kfree(void *);
extern int __kfree_by_pid(pid_t);

addr_t alloc_pages(gfp_t flag, u32 order)
{
    void *ptr = NULL;
    (void)flag;
    if (posix_memalign(&ptr, CONFIG_PAGE_SIZE, (size_t)CONFIG_PAGE_SIZE << order) != 0) return 0;
    return (addr_t)ptr;
}
int free_pages(addr_t addr, u32 order) { (void)order; free((void *)addr); return 0; }

static void *worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (int round = 0; round < ROUNDS; round++) {
        size_t size = 1 + ((round * 37 + id * 101) % 700);
        unsigned char byte = (unsigned char)(id + 1);
        char *p = __kalloc((u32)size, 0, (pid_t)(id + 1));
        assert(p != NULL);
        pthread_mutex_lock(&live_lock);
        for (int j = 0; j < THREADS; j++) {
            if (live[j].ptr != NULL) {
                assert((uintptr_t)p + size <= (uintptr_t)live[j].ptr ||
                       (uintptr_t)live[j].ptr + live[j].size <= (uintptr_t)p);
            }
        }
        live[id] = (struct live){ p, size };
        pthread_mutex_unlock(&live_lock);
        memset(p, byte, size);
        sched_yield();
        for (size_t i = 0; i < size; i++) assert((unsigned char)p[i] == byte);
        pthread_mutex_lock(&live_lock);
        live[id].ptr = NULL;
        assert(__kfree(p) == 0);
        pthread_mutex_unlock(&live_lock);
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[THREADS];
    assert(__kalloc(0, 0, 1) == NULL);
    assert(__kalloc(U32_MAX, 0, 1) == NULL);
    {
        char *first = __kalloc(64, 0, 11);
        char *other = __kalloc(64, 0, 12);
        char *last = __kalloc(64, 0, 11);
        assert(first && other && last);
        memset(other, 0x5a, 64);
        assert(__kfree_by_pid(11) == 0);
        for (int i = 0; i < 64; i++) assert((unsigned char)other[i] == 0x5a);
        assert(__kfree(first) == -EINVAL);
        assert(__kfree(last) == -EINVAL);
        assert(__kfree(other) == 0);
    }
    for (uintptr_t i = 0; i < THREADS; i++) assert(pthread_create(&threads[i], NULL, worker, (void *)i) == 0);
    for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
    puts("concurrent memory block allocation checks passed");
    return 0;
}
