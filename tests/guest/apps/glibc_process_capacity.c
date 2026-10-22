/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILDREN 25u
#define PAGE_BYTES 4096u
struct child_ready {
    int32_t pid;
    uint32_t index, cpu_mask, byte, error;
};
static volatile unsigned private_word = 0x7152;
static _Thread_local unsigned private_tls = 0x4821;
static unsigned char *protected_page, *heap_page;

static int fail(const char *name, unsigned detail)
{
    fprintf(stderr, "FAIL %s detail=%u errno=%d\n", name, detail, errno);
    return 97;
}
static int read_ready(int fd, struct child_ready *ready)
{
    unsigned char *bytes = (unsigned char *)ready;
    size_t copied = 0;
    while (copied < sizeof(*ready)) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        int rc;
        do { rc = poll(&descriptor, 1, 30000); } while (rc < 0 && errno == EINTR);
        if (rc <= 0 || !(descriptor.revents & (POLLIN | POLLHUP))) return 0;
        ssize_t count = read(fd, bytes + copied, sizeof(*ready) - copied);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        copied += (size_t)count;
    }
    return 1;
}
static pid_t reap(pid_t pid, int *status)
{
    pid_t result;
    do { result = waitpid(pid, status, 0); } while (result < 0 && errno == EINTR);
    return result;
}
static void child_work(unsigned index, unsigned wave, int fd, int ready_pipe[2], int release_pipe[2])
{
    close(ready_pipe[0]); close(release_pipe[1]);
    struct child_ready ready = {.pid = (int32_t)getpid(), .index = index};
    struct stat info;
    char positioned = 0, shared = 0;
    if (private_word != 0x7152 || private_tls != 0x4821 || heap_page[0] != 37 || protected_page[0] != 37)
        ready.error = 1;
    private_word += index + wave * 100; private_tls += index + wave * 100;
    heap_page[0] = (unsigned char)(index + 1);
    /* A private mapping which was read-only at fork may later become
     * writable in one child. It must retain private backing semantics. */
    if (mprotect(protected_page, PAGE_BYTES, PROT_READ | PROT_WRITE)) ready.error = 2;
    else {
        protected_page[0] = (unsigned char)(index + 1);
        if (mprotect(protected_page, PAGE_BYTES, PROT_READ)) ready.error = 3;
    }
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size != CHILDREN ||
        pread(fd, &positioned, 1, (off_t)index) != 1 || positioned != (char)('A' + index) ||
        read(fd, &shared, 1) != 1 || shared < 'A' || shared >= 'A' + (int)CHILDREN) ready.error = 4;
    ready.byte = (unsigned char)shared;
    for (unsigned round = 0; round < 16; round++) {
        int cpu = sched_getcpu();
        if (cpu < 0 || cpu >= 32) ready.error = 5;
        else ready.cpu_mask |= 1u << cpu;
        sched_yield();
    }
    ssize_t sent;
    do { sent = write(ready_pipe[1], &ready, sizeof(ready)); } while (sent < 0 && errno == EINTR);
    close(ready_pipe[1]);
    if (sent != (ssize_t)sizeof(ready)) _exit(110);
    char release;
    ssize_t count;
    do { count = read(release_pipe[0], &release, 1); } while (count < 0 && errno == EINTR);
    close(release_pipe[0]); close(fd);
    if (!count) _exit(0); /* The parent is unwinding a partial-creation error. */
    if (count != 1 || release != 'G' || ready.error || private_word != 0x7152 + index + wave * 100 ||
        private_tls != 0x4821 + index + wave * 100 || heap_page[0] != (unsigned char)(index + 1) ||
        protected_page[0] != (unsigned char)(index + 1)) _exit(111);
    _exit((int)(index + 1));
}
static int wave(unsigned number, unsigned target, int fd, unsigned expected_cpus)
{
    int ready_pipe[2] = {-1, -1}, release_pipe[2] = {-1, -1};
    pid_t children[CHILDREN] = {0};
    unsigned made = 0, failure = 0, cpu_mask = 0;
    uint32_t seen = 0, bytes = 0;
    if (lseek(fd, 0, SEEK_SET) || pipe(ready_pipe)) return fail("glibc_process_capacity_setup", 1);
    if (pipe(release_pipe)) { close(ready_pipe[0]); close(ready_pipe[1]); return fail("glibc_process_capacity_setup", 2); }
    for (; made < target; made++) {
        children[made] = fork();
        if (!children[made]) child_work(made, number, fd, ready_pipe, release_pipe);
        if (children[made] < 0) {
            int error = errno;
            printf("glibc_process_capacity_partial wave=%u created=%u errno=%d\n", number, made, error);
            failure = 10; break;
        }
    }
    close(ready_pipe[1]); ready_pipe[1] = -1;
    close(release_pipe[0]); release_pipe[0] = -1;
    if (!failure) for (unsigned i = 0; i < made; i++) {
        struct child_ready ready;
        if (!read_ready(ready_pipe[0], &ready)) { failure = 11; break; }
        if (ready.index >= made || ready.pid != children[ready.index] || ready.error ||
            (seen & (1u << ready.index)) || !ready.cpu_mask || (ready.cpu_mask & ~((1u << expected_cpus) - 1u)) ||
            ready.byte < 'A' || ready.byte >= 'A' + made || (bytes & (1u << (ready.byte - 'A')))) {
            failure = 12; break;
        }
        for (unsigned j = 0; j < ready.index; j++) if (children[j] == ready.pid) failure = 13;
        seen |= 1u << ready.index; bytes |= 1u << (ready.byte - 'A'); cpu_mask |= ready.cpu_mask;
    }
    if (!failure) {
        uint32_t expected = (1u << target) - 1u;
        if (seen != expected || bytes != expected || lseek(fd, 0, SEEK_CUR) != (off_t)target ||
            private_word != 0x7152 || private_tls != 0x4821 || heap_page[0] != 37 || protected_page[0] != 37 ||
            (expected_cpus > 1 && !(cpu_mask & (cpu_mask - 1u)))) failure = 14;
        for (unsigned i = 0; i < made; i++) {
            int status;
            if (waitpid(children[i], &status, WNOHANG) != 0) failure = 15;
        }
    }
    if (!failure) {
        /* All children have sent readiness but cannot exit until the parent
         * releases them. The native monitor independently samples this hold. */
        struct timespec hold = {.tv_nsec = 100000000};
        while (nanosleep(&hold, &hold) && errno == EINTR) {}
        printf("glibc_process_capacity_wave=%u children=%u live=%u cpu_mask=%x\n", number, made, made + 1, cpu_mask);
        char release[CHILDREN]; memset(release, 'G', sizeof(release));
        ssize_t count;
        do { count = write(release_pipe[1], release, made); } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)made) failure = 16;
    }
    /* Closing the last writer releases every survivor on a failed fork or
     * setup assertion too. No partial capacity probe leaves blocked children. */
    close(release_pipe[1]); close(ready_pipe[0]);
    for (unsigned i = 0; i < made; i++) {
        int status;
        if (reap(children[i], &status) != children[i]) failure = 17;
        else if (!failure && (!WIFEXITED(status) || WEXITSTATUS(status) != (int)(i + 1))) failure = 18;
    }
    if (failure) return fail("glibc_process_capacity_wave", number * 100 + failure);
    if (target == CHILDREN) {
        if (number == 1) puts("PASS glibc_process_capacity_first25_live");
        else puts("PASS glibc_process_capacity_second25_live");
    }
    return 0;
}
struct readonly_reader {
    unsigned char *address;
    unsigned seed, stop, ready, loads, error, saw_old, saw_new;
    int last_cpu;
};
static void *readonly_reader(void *argument)
{
    struct readonly_reader *state = argument;
    unsigned loads = 0;
    while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) {
        unsigned byte = __atomic_load_n(state->address, __ATOMIC_RELAXED);
        if (byte == state->seed) __atomic_store_n(&state->saw_old, 1, __ATOMIC_RELEASE);
        else if (byte == state->seed + 1) __atomic_store_n(&state->saw_new, 1, __ATOMIC_RELEASE);
        else __atomic_store_n(&state->error, 1, __ATOMIC_RELEASE);
        if (!(++loads & 255u)) {
            int cpu = sched_getcpu();
            if (cpu < 0 || cpu >= 32) __atomic_store_n(&state->error, 2, __ATOMIC_RELEASE);
            __atomic_store_n(&state->last_cpu, cpu, __ATOMIC_RELEASE);
            __atomic_store_n(&state->loads, loads, __ATOMIC_RELEASE);
            __atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&state->loads, loads, __ATOMIC_RELEASE);
    return (void *)(uintptr_t)__atomic_load_n(&state->error, __ATOMIC_ACQUIRE);
}
static int readonly_wait(struct readonly_reader *state, unsigned cpus, unsigned before, int after)
{
    struct timespec limit, now;
    if (clock_gettime(CLOCK_MONOTONIC, &limit)) return 0;
    limit.tv_sec += 3;
    for (;;) {
        if (__atomic_load_n(&state->error, __ATOMIC_ACQUIRE)) return 0;
        unsigned ready = __atomic_load_n(&state->ready, __ATOMIC_ACQUIRE);
        unsigned observed = __atomic_load_n(after ? &state->saw_new : &state->saw_old, __ATOMIC_ACQUIRE);
        unsigned loads = __atomic_load_n(&state->loads, __ATOMIC_ACQUIRE);
        int remote = after || cpus == 1;
        if (!remote) {
            int own_cpu = sched_getcpu();
            int peer_cpu = __atomic_load_n(&state->last_cpu, __ATOMIC_ACQUIRE);
            if (own_cpu < 0 || own_cpu >= (int)cpus) return 0;
            remote = peer_cpu >= 0 && peer_cpu < (int)cpus && peer_cpu != own_cpu;
        }
        if (ready && observed && remote && loads > before) return 1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec > limit.tv_sec ||
            (now.tv_sec == limit.tv_sec && now.tv_nsec >= limit.tv_nsec)) return 0;
        sched_yield();
    }
}
static void readonly_child(unsigned iteration, unsigned seed, unsigned char *memory, int ready_pipe[2], int release_pipe[2])
{
    close(ready_pipe[0]); close(release_pipe[1]);
    struct child_ready ready = {.pid = (int32_t)getpid(), .index = iteration};
    for (unsigned page = 0; page < 3; page++) if (memory[page * PAGE_BYTES] != seed) ready.error = 1;
    ssize_t count;
    do { count = write(ready_pipe[1], &ready, sizeof(ready)); } while (count < 0 && errno == EINTR);
    close(ready_pipe[1]);
    if (count != (ssize_t)sizeof(ready)) _exit(120);
    char release = 0;
    do { count = read(release_pipe[0], &release, 1); } while (count < 0 && errno == EINTR);
    close(release_pipe[0]);
    if (!count) _exit(0);
    if (count != 1 || release != 'G' || ready.error) _exit(121);
    for (unsigned page = 0; page < 3; page++) if (memory[page * PAGE_BYTES] != seed) _exit(122);
    if (mprotect(memory + PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE)) _exit(123);
    memory[PAGE_BYTES] = (unsigned char)(seed + 2);
    if (memory[0] != seed || memory[2 * PAGE_BYTES] != seed ||
        munmap(memory + PAGE_BYTES, PAGE_BYTES) || memory[0] != seed || memory[2 * PAGE_BYTES] != seed) _exit(124);
    _exit(0);
}
static int readonly_upgrades(unsigned cpus)
{
    enum { ITERATIONS = 16 };
    unsigned long long total_loads = 0;
    unsigned remote_iterations = 0;
    for (unsigned iteration = 0; iteration < ITERATIONS; iteration++) {
        unsigned seed = 37 + iteration;
        unsigned char *memory = mmap(NULL, 3 * PAGE_BYTES, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return fail("glibc_process_capacity_readonly_mapping", iteration);
        memset(memory, (int)seed, 3 * PAGE_BYTES);
        if (mprotect(memory, 3 * PAGE_BYTES, PROT_READ)) return fail("glibc_process_capacity_readonly_setup", iteration);
        int ready_pipe[2], release_pipe[2];
        if (pipe(ready_pipe)) return fail("glibc_process_capacity_readonly_pipe", 1);
        if (pipe(release_pipe)) {
            close(ready_pipe[0]); close(ready_pipe[1]);
            return fail("glibc_process_capacity_readonly_pipe", 2);
        }
        pid_t child = fork();
        if (!child) readonly_child(iteration, seed, memory, ready_pipe, release_pipe);
        if (child < 0) {
            close(ready_pipe[0]); close(ready_pipe[1]); close(release_pipe[0]); close(release_pipe[1]);
            munmap(memory, 3 * PAGE_BYTES);
            return fail("glibc_process_capacity_readonly_fork", iteration);
        }
        close(ready_pipe[1]); close(release_pipe[0]);
        struct child_ready ready;
        unsigned failure = !read_ready(ready_pipe[0], &ready) || ready.pid != child ||
            ready.index != iteration || ready.error;
        struct readonly_reader state = {.address = memory + PAGE_BYTES, .seed = seed, .last_cpu = -1};
        pthread_t reader;
        int started = 0;
        if (!failure) {
            int rc = pthread_create(&reader, NULL, readonly_reader, &state);
            if (rc) failure = 2;
            else started = 1;
        }
        if (!failure && !readonly_wait(&state, cpus, 0, 0)) failure = 3;
        unsigned before = __atomic_load_n(&state.loads, __ATOMIC_ACQUIRE);
        if (!failure) {
            if (cpus > 1) remote_iterations++;
            /* READ remains valid throughout both permission changes. A live
             * reader on another CPU must not observe a BBM translation hole,
             * the child's old physical backing, or unexpected data. */
            if (mprotect(memory + PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE)) failure = 4;
            else {
                __atomic_store_n(memory + PAGE_BYTES, (unsigned char)(seed + 1), __ATOMIC_RELEASE);
                if (mprotect(memory + PAGE_BYTES, PAGE_BYTES, PROT_READ)) failure = 5;
            }
        }
        if (!failure && !readonly_wait(&state, cpus, before, 1)) failure = 6;
        if (started) {
            __atomic_store_n(&state.stop, 1, __ATOMIC_RELEASE);
            void *result = NULL;
            if (pthread_join(reader, &result) || result) failure = 7;
            total_loads += __atomic_load_n(&state.loads, __ATOMIC_ACQUIRE);
        }
        if (!failure && (memory[0] != seed || memory[PAGE_BYTES] != seed + 1 ||
                         memory[2 * PAGE_BYTES] != seed)) failure = 8;
        if (!failure) {
            ssize_t count;
            do { count = write(release_pipe[1], "G", 1); } while (count < 0 && errno == EINTR);
            if (count != 1) failure = 9;
        }
        close(release_pipe[1]); close(ready_pipe[0]);
        int status;
        if (reap(child, &status) != child || !WIFEXITED(status) || WEXITSTATUS(status)) failure = 10;
        if (!failure && (memory[0] != seed || memory[PAGE_BYTES] != seed + 1 ||
                         memory[2 * PAGE_BYTES] != seed)) failure = 11;
        if (munmap(memory, 3 * PAGE_BYTES)) failure = 12;
        if (failure) return fail("glibc_process_capacity_readonly_upgrade", iteration * 100 + failure);
    }
    printf("glibc_process_capacity_readonly_iterations=%u reader_loads=%llu remote_iterations=%u\n",
           ITERATIONS, total_loads, remote_iterations);
    puts("PASS glibc_process_capacity_readonly_upgrade_smp");
    return 0;
}
int main(int argc, char **argv)
{
    alarm(90);
    unsigned target = CHILDREN;
    if (argc == 3 && !strcmp(argv[1], "probe")) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (*end || value < 1 || value > CHILDREN) return fail("glibc_process_capacity_probe_argument", 0);
        target = (unsigned)value;
    }
    const char *configured = getenv("NOS_EXPECT_CPUS");
    unsigned expected_cpus = configured ? (unsigned)atoi(configured) : 1;
    if (expected_cpus != 1 && expected_cpus != 2 && expected_cpus != 4) return fail("glibc_process_capacity_cpus", expected_cpus);
    protected_page = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    heap_page = malloc(PAGE_BYTES);
    if (protected_page == MAP_FAILED || !heap_page) return fail("glibc_process_capacity_memory", 0);
    memset(protected_page, 37, PAGE_BYTES); memset(heap_page, 37, PAGE_BYTES);
    if (mprotect(protected_page, PAGE_BYTES, PROT_READ)) return fail("glibc_process_capacity_protect", 0);
    int fd = open("/tmp/glibc-process-capacity", O_CREAT | O_TRUNC | O_RDWR, 0600);
    char contents[CHILDREN];
    for (unsigned i = 0; i < CHILDREN; i++) contents[i] = (char)('A' + i);
    if (fd < 0 || write(fd, contents, sizeof(contents)) != (ssize_t)sizeof(contents)) return fail("glibc_process_capacity_file", 0);
    int rc = wave(1, target, fd, expected_cpus);
    if (!rc) rc = wave(2, target, fd, expected_cpus);
    if (close(fd) || unlink("/tmp/glibc-process-capacity") || munmap(protected_page, PAGE_BYTES))
        return fail("glibc_process_capacity_cleanup", 0);
    free(heap_page);
    if (rc) return rc;
    rc = readonly_upgrades(expected_cpus);
    if (rc) return rc;
    if (target == CHILDREN) {
        puts("PASS glibc_process_capacity_private_memory");
        puts("PASS glibc_process_capacity_shared_ofd");
        puts("PASS glibc_process_capacity_wait_status");
        puts("PASS glibc_process_capacity_complete");
    } else printf("glibc_process_capacity_probe_complete children=%u\n", target);
    return 0;
}
