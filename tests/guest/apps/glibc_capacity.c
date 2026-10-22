/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 64u
#define ROUNDS 8u
#define DEFAULT_STACK (256u * 1024u)
/* Slot decoding follows the Linux compatibility profile, not a libc limit. */
#define TASK_SLOTS 80u
struct worker_state {
    pthread_t thread;
    uintptr_t stack;
    size_t stack_size;
    pid_t tid;
    unsigned identity, error, signals;
};
static struct worker_state workers[WORKERS];
static uintptr_t previous_stacks[WORKERS];
static _Thread_local unsigned initialized_tls = 0x7152;
static _Thread_local unsigned zero_tls;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t key;
static unsigned ready, go, destructors, cpu_mask, operations;
static FILE *shared_stream;
static char stream_buffer[1024];
static struct rlimit original_nofile;

static int fail(const char *name, int error)
{
    fprintf(stderr, "FAIL %s (%d)\n", name, error);
    return 97;
}
static void destructor(void *value)
{
    if (value && (uintptr_t)value <= WORKERS)
        __atomic_add_fetch(&destructors, 1, __ATOMIC_RELEASE);
}
static void signal_handler(int signal)
{
    if (signal == SIGUSR1 && zero_tls && zero_tls <= WORKERS)
        __atomic_add_fetch(&workers[zero_tls - 1].signals, 1, __ATOMIC_RELEASE);
}
static void *worker(void *argument)
{
    struct worker_state *state = argument;
    unsigned identity = state->identity;
    pthread_attr_t attributes;
    void *stack = NULL;
    size_t stack_size = 0;
    struct rlimit nofile;
    state->tid = (pid_t)syscall(SYS_gettid);
    if (initialized_tls != 0x7152 || zero_tls ||
        pthread_setspecific(key, (void *)(uintptr_t)identity)) state->error = 1;
    initialized_tls += identity; zero_tls = identity;
    int rc = pthread_getattr_np(pthread_self(), &attributes);
    if (rc) state->error = 2;
    else {
        if (pthread_attr_getstack(&attributes, &stack, &stack_size)) state->error = 3;
        pthread_attr_destroy(&attributes);
    }
    state->stack = (uintptr_t)stack; state->stack_size = stack_size;
    if (getrlimit(RLIMIT_NOFILE, &nofile) || nofile.rlim_cur != original_nofile.rlim_cur ||
        nofile.rlim_max != original_nofile.rlim_max || getuid() != 0) state->error = 4;
    if (pthread_mutex_lock(&lock)) return (void *)0x1001;
    ready++; pthread_cond_broadcast(&condition);
    while (!go) if (pthread_cond_wait(&condition, &lock)) return (void *)0x1002;
    pthread_mutex_unlock(&lock);
    if (state->error) return (void *)0x1003;
    /* Every worker, including slots above 32 and 64, makes a real VFS RPC.
     * Buffered fputc alone would only exercise glibc's local FILE lock. */
    struct stat file;
    if (fstat(fileno(shared_stream), &file) || !S_ISREG(file.st_mode)) return (void *)0x100a;
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned char *memory = malloc(4096);
        if (!memory) return (void *)0x1004;
        memset(memory, (int)(identity + round), 4096);
        int cpu = sched_getcpu();
        if (cpu < 0 || cpu >= 32) { free(memory); return (void *)0x1005; }
        __atomic_fetch_or(&cpu_mask, 1u << cpu, __ATOMIC_RELAXED);
        if (pthread_mutex_lock(&lock)) { free(memory); return (void *)0x1006; }
        operations++;
        pthread_mutex_unlock(&lock);
        if (fputc((int)identity, shared_stream) == EOF) { free(memory); return (void *)0x1007; }
        /* Successful allocation and stdio may change errno. Only the yield
         * window is part of this thread-local errno assertion. */
        errno = 1000 + (int)identity;
        sched_yield();
        for (unsigned i = 0; i < 4096; i++)
            if (memory[i] != (unsigned char)(identity + round)) { free(memory); return (void *)0x1008; }
        if (errno != 1000 + (int)identity || initialized_tls != 0x7152 + identity || zero_tls != identity) {
            free(memory); return (void *)0x1009;
        }
        free(memory);
    }
    return (void *)(uintptr_t)identity;
}
static int wave(unsigned number)
{
    ready = go = operations = 0;
    unsigned previous_destructors = __atomic_load_n(&destructors, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < WORKERS; i++) {
        workers[i] = (struct worker_state){.identity = i + 1};
        int rc = pthread_create(&workers[i].thread, NULL, worker, &workers[i]);
        if (rc) return fail("glibc_capacity_default_create", rc);
    }
    if (pthread_mutex_lock(&lock)) return fail("glibc_capacity_lock", 1);
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return fail("glibc_capacity_clock", errno);
    deadline.tv_sec += 30;
    while (ready != WORKERS) {
        int rc = pthread_cond_timedwait(&condition, &lock, &deadline);
        if (rc) return fail("glibc_capacity_ready", rc);
    }
    unsigned high_slot = WORKERS, reused = 0;
    for (unsigned i = 0; i < WORKERS; i++) {
        if (workers[i].error || workers[i].tid <= 0 || !workers[i].stack ||
            workers[i].stack_size != DEFAULT_STACK)
            return fail("glibc_capacity_worker_setup", (int)(i + 1));
        for (unsigned j = 0; j < i; j++)
            if (workers[i].tid == workers[j].tid || workers[i].stack == workers[j].stack)
                return fail("glibc_capacity_unique_live", (int)i);
        if ((unsigned)workers[i].tid % TASK_SLOTS >= 64) high_slot = i;
        for (unsigned j = 0; number == 2 && j < WORKERS; j++)
            if (workers[i].stack == previous_stacks[j]) { reused++; break; }
    }
    if (high_slot == WORKERS) return fail("glibc_capacity_high_slot", 0);
    pthread_mutex_unlock(&lock);
    if (pthread_kill(workers[high_slot].thread, SIGUSR1)) return fail("glibc_capacity_high_signal", 1);
    /* Keep the entire cohort alive for independent native-parent sampling.
     * This also gives a directed signal time to interrupt a high-slot waiter. */
    struct timespec hold = {.tv_nsec = 100000000};
    while (nanosleep(&hold, &hold) && errno == EINTR) {}
    if (__atomic_load_n(&workers[high_slot].signals, __ATOMIC_ACQUIRE) != 1)
        return fail("glibc_capacity_high_signal", 2);
    if (number == 2 && !reused) return fail("glibc_capacity_stack_cache", 0);
    printf("glibc_capacity_wave=%u live=65 stack_bytes=%u reused=%u\n", number, DEFAULT_STACK, reused);
    pthread_mutex_lock(&lock);
    go = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < WORKERS; i++) {
        void *result = NULL;
        int rc = pthread_join(workers[i].thread, &result);
        if (rc || result != (void *)(uintptr_t)(i + 1))
            return fail("glibc_capacity_join", rc ? rc : (int)(uintptr_t)result);
        if (number == 1) previous_stacks[i] = workers[i].stack;
    }
    if (__atomic_load_n(&destructors, __ATOMIC_ACQUIRE) != previous_destructors + WORKERS ||
        operations != WORKERS * ROUNDS || initialized_tls != 0x7152 || zero_tls)
        return fail("glibc_capacity_tls_destructors", (int)operations);
    if (fflush(shared_stream) || fseek(shared_stream, 0, SEEK_SET)) return fail("glibc_capacity_stdio", errno);
    unsigned histogram[WORKERS] = {0};
    for (unsigned i = 0; i < WORKERS * ROUNDS; i++) {
        int byte = fgetc(shared_stream);
        if (byte < 1 || byte > (int)WORKERS) return fail("glibc_capacity_stdio_data", byte);
        histogram[(unsigned)byte - 1]++;
    }
    if (fgetc(shared_stream) != EOF || ferror(shared_stream)) return fail("glibc_capacity_stdio_length", errno);
    for (unsigned i = 0; i < WORKERS; i++)
        if (histogram[i] != ROUNDS) return fail("glibc_capacity_stdio_histogram", (int)i);
    if (ftruncate(fileno(shared_stream), 0) || fseek(shared_stream, 0, SEEK_SET))
        return fail("glibc_capacity_stdio_reset", errno);
    if (number == 1) puts("PASS glibc_capacity_first_64_live");
    else puts("PASS glibc_capacity_second_64_live");
    return 0;
}
int main(void)
{
    /* No pthread stack attributes, allocator tunables or sysconf overrides.
     * glibc derives its genuine default stack from the startup STACK limit. */
    alarm(90);
    pthread_attr_t attributes;
    size_t stack_size;
    struct rlimit stack_limit;
    if (pthread_attr_init(&attributes) || pthread_attr_getstacksize(&attributes, &stack_size) ||
        pthread_attr_destroy(&attributes) || getrlimit(RLIMIT_STACK, &stack_limit) ||
        stack_size != DEFAULT_STACK || stack_limit.rlim_cur != DEFAULT_STACK)
        return fail("glibc_capacity_default_stack", 0);
    puts("PASS glibc_capacity_default_stack");
    if (getrlimit(RLIMIT_NOFILE, &original_nofile) || pthread_key_create(&key, destructor))
        return fail("glibc_capacity_setup", errno);
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) return fail("glibc_capacity_signal_setup", errno);
    shared_stream = fopen("glibc-capacity", "w+");
    if (!shared_stream || setvbuf(shared_stream, stream_buffer, _IOFBF, sizeof(stream_buffer)))
        return fail("glibc_capacity_stream", errno);
    int rc = wave(1);
    if (rc) return rc;
    rc = wave(2);
    if (rc) return rc;
    puts("PASS glibc_capacity_default_stack_cache_reuse");
    puts("PASS glibc_capacity_high_slot_signal_futex_vfs");
    puts("PASS glibc_capacity_tls_errno_malloc_stdio");
    puts("PASS glibc_capacity_tsd_join_reclaim");
    if (fclose(shared_stream) || unlink("glibc-capacity") || pthread_key_delete(key))
        return fail("glibc_capacity_cleanup", errno);
    printf("glibc_capacity_cpu_mask=%x\n", __atomic_load_n(&cpu_mask, __ATOMIC_RELAXED));
    puts("PASS glibc_capacity_complete");
    return 0;
}
