/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include "abi.h"
#include "user.h"
#include "thread.h"
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
static int select_directory_item(const struct dirent *entry)
{ return !strcmp(entry->d_name, "item"); }
static void posix_directory_regression(void)
{
    char old_cwd[64] = "/", cwd[64], data[8] = { 0 };
    int ok = getcwd(old_cwd, sizeof(old_cwd)) != NULL;
    ok &= nos_mkdir("/pd") == 0;
    int directory = open("/pd", O_RDONLY);
    int fd = directory < 0 ? -1 : openat(directory, "item", O_CREAT | O_TRUNC | O_RDWR, 0600);
    ok &= directory >= 0 && fd >= 0;
    if (fd >= 0) {
        ok &= write(fd, "abcdef", 6) == 6 && lseek(fd, 2, SEEK_SET) == 2;
        ok &= pread(fd, data, 3, 1) == 3 && !memcmp(data, "bcd", 3) && lseek(fd, 0, SEEK_CUR) == 2;
        ok &= pwrite(fd, "XY", 2, 3) == 2 && lseek(fd, 0, SEEK_CUR) == 2;
        ok &= pread(fd, data, 6, 0) == 6 && !memcmp(data, "abcXYf", 6);
        struct stat st;
        ok &= !fstatat(directory, "item", &st, 0) && S_ISREG(st.st_mode) && st.st_size == 6 && st.st_ino;
        close(fd);
    }
    nos_check(ok, "posix_openat_positioned_io_stat");
    int cwd_ok = !chdir("/pd") && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/pd");
    fd = open("item", O_RDONLY);
    cwd_ok &= fd >= 0;
    if (fd >= 0) close(fd);
    cwd_ok &= !chdir("/") && !fchdir(directory) && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/pd");
    cwd_ok &= !chdir(old_cwd);
    nos_check(cwd_ok, "posix_cwd_relative_paths");
    DIR *stream = opendir("/pd");
    int found = 0, directory_ok = stream != NULL;
    if (stream) {
        directory_ok &= (fcntl(dirfd(stream), F_GETFD) & FD_CLOEXEC) != 0;
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(stream))) if (!strcmp(entry->d_name, "item")) found++;
        directory_ok &= errno == 0 && found == 1;
        rewinddir(stream);
        entry = readdir(stream);
        directory_ok &= entry != NULL;
        if (entry) {
            long position = telldir(stream);
            struct dirent *next = readdir(stream);
            char expected[64];
            if (next) {
                strcpy(expected, next->d_name);
                seekdir(stream, position);
                next = readdir(stream);
                directory_ok &= next && !strcmp(expected, next->d_name);
            }
        }
        directory_ok &= !closedir(stream);
    }
    struct dirent **selected = NULL;
    int selected_count = scandir("/pd", &selected, select_directory_item, alphasort);
    directory_ok &= selected_count == 1 && selected && !strcmp(selected[0]->d_name, "item");
    if (selected_count >= 0) {
        for (int i = 0; i < selected_count; i++) free(selected[i]);
        free(selected);
    }
    nos_check(directory_ok, "posix_directory_iteration_seek");
    if (directory >= 0) close(directory);
    nos_unlink("/pd/item"); nos_rmdir("/pd");

    char long_path[63];
    long_path[0] = '/';
    memset(long_path + 1, 'q', 60); long_path[61] = 0;
    fd = open(long_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
    stream = opendir("/");
    found = 0;
    int long_ok = fd >= 0 && stream != NULL;
    if (stream) {
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(stream))) if (!strcmp(entry->d_name, long_path + 1)) found++;
        long_ok &= !errno && found == 1 && !closedir(stream);
    }
    nos_check(long_ok, "posix_directory_long_record");
    nos_unlink(long_path);
}
static void posix_poll_regression(void)
{
    int descriptors[2];
    int ok = !pipe(descriptors);
    if (!ok) { nos_check(0, "posix_poll_select_readiness"); return; }
    struct pollfd query[2] = {{descriptors[0], POLLIN, -1}, {descriptors[1], POLLOUT, -1}};
    ok &= poll(query, 2, 0) == 1 && !query[0].revents && (query[1].revents & POLLOUT);
    ok &= write(descriptors[1], "abc", 3) == 3 && poll(query, 2, 0) == 2 && (query[0].revents & POLLIN);
    fd_set input, output;
    FD_ZERO(&input); FD_ZERO(&output);
    FD_SET(descriptors[0], &input); FD_SET(descriptors[1], &output);
    struct timeval zero = { 0, 0 };
    ok &= select(descriptors[1] + 1, &input, &output, NULL, &zero) == 2 &&
        FD_ISSET(descriptors[0], &input) && FD_ISSET(descriptors[1], &output);
    struct timespec immediate = { 0, 0 };
    ok &= ppoll(query, 2, &immediate, NULL) == 2;
    FD_ZERO(&input); FD_SET(descriptors[0], &input);
    ok &= pselect(descriptors[0] + 1, &input, NULL, NULL, &immediate, NULL) == 1 && FD_ISSET(descriptors[0], &input);
    char bytes[8];
    ok &= read(descriptors[0], bytes, 3) == 3 && !memcmp(bytes, "abc", 3);
    nos_check(ok, "posix_poll_select_readiness");
    struct timespec before = { 0, 0 }, after = { 0, 0 };
    ok = !clock_gettime(CLOCK_MONOTONIC, &before) && poll(query, 1, 25) == 0 &&
        !clock_gettime(CLOCK_MONOTONIC, &after);
    int64_t elapsed = (int64_t)(after.tv_sec - before.tv_sec) * 1000000000 + after.tv_nsec - before.tv_nsec;
    ok &= elapsed >= 20000000 && !close(descriptors[1]) && poll(query, 1, 0) == 1 &&
        (query[0].revents & POLLHUP) && read(descriptors[0], bytes, sizeof(bytes)) == 0;
    struct pollfd invalid[2] = {{32767, POLLIN, 0}, {-1, POLLIN, -1}};
    ok &= poll(invalid, 2, 0) == 1 && (invalid[0].revents & POLLNVAL) && !invalid[1].revents;
    FD_ZERO(&input); FD_SET(31, &input); zero = (struct timeval){0, 0}; errno = 0;
    ok &= select(32, &input, NULL, NULL, &zero) == -1 && errno == EBADF;
    ok &= !close(descriptors[0]);
    nos_check(ok, "posix_poll_timeout_eof_invalid");
}
static void posix_metadata_regression(void)
{
    mode_t previous = umask(0027);
    int fd = open("/meta", O_CREAT | O_RDWR | O_TRUNC, 0666);
    struct stat st = { 0 };
    int ok = fd >= 0 && !fstat(fd, &st) && (st.st_mode & 0777) == 0640;
    if (fd >= 0) {
        char bytes[8];
        ok &= write(fd, "abcd", 4) == 4 && lseek(fd, 2, SEEK_SET) == 2 && !ftruncate(fd, 8) &&
            lseek(fd, 0, SEEK_CUR) == 2 && pread(fd, bytes, 8, 0) == 8 && !memcmp(bytes, "abcd\0\0\0\0", 8);
        ok &= !truncate("/meta", 2) && !fstat(fd, &st) && st.st_size == 2;
        ok &= !chmod("/meta", 0600) && !fstat(fd, &st) && (st.st_mode & 0777) == 0600;
        ok &= !fchmod(fd, 0644) && !fstat(fd, &st) && (st.st_mode & 0777) == 0644;
        ok &= !close(fd);
    }
    umask(previous);
    nos_check(ok, "posix_metadata_modes_truncate");
    struct stat alias = { 0 };
    ok = !link("/meta", "/hard") && !stat("/meta", &st) && !stat("/hard", &alias) &&
        st.st_ino == alias.st_ino && st.st_nlink == 2 && alias.st_nlink == 2;
    ok &= !symlink("hard", "/sym");
    char target[8] = { 0 };
    ok &= readlink("/sym", target, sizeof(target)) == 4 && !memcmp(target, "hard", 4) &&
        !lstat("/sym", &st) && S_ISLNK(st.st_mode) && !stat("/sym", &st) && S_ISREG(st.st_mode) && st.st_ino == alias.st_ino;
    ok &= !unlink("/meta");
    fd = open("/hard", O_RDONLY);
    if (fd >= 0) { ok &= read(fd, target, 2) == 2 && !memcmp(target, "ab", 2); close(fd); } else ok = 0;
    ok &= !unlink("/hard");
    errno = 0;
    ok &= stat("/sym", &st) == -1 && errno == ENOENT && !lstat("/sym", &st) && S_ISLNK(st.st_mode);
    ok &= !unlink("/sym");
    nos_check(ok, "posix_metadata_links_symlinks");
}
#endif
#if defined(NOS_MUSL) || defined(NOS_NEWLIB)
#include <pthread.h>
#endif
#ifdef NOS_MUSL
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

#ifndef NOS_MUSL
static struct nos_lock native_lock;
static struct nos_condition native_condition;
static int native_ready, native_go, native_count;
static void *native_worker(void *argument)
{
    if (nos_lock_acquire(&native_lock)) return (void *)-1;
    native_ready++;
    nos_condition_signal(&native_condition, 1);
    while (!native_go)
        if (nos_condition_wait(&native_condition, &native_lock)) return (void *)-1;
    nos_lock_release(&native_lock);
    for (unsigned i = 0; i < 64; i++) {
        if (nos_lock_acquire(&native_lock)) return (void *)-1;
        native_count++;
        nos_lock_release(&native_lock);
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
        errno = (int)(uintptr_t)argument;
        char *block = malloc(96);
        if (!block) return (void *)-1;
        for (unsigned j = 0; j < 96; j++) block[j] = (char)(i + j);
        nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
        for (unsigned j = 0; j < 96; j++)
            if (block[j] != (char)(i + j)) return (void *)-1;
        if (errno != (int)(uintptr_t)argument) return (void *)-1;
        free(block);
#else
        nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
#endif
    }
    return (void *)((uintptr_t)argument + 1);
}
#endif
#if defined(NOS_MUSL) || defined(NOS_NEWLIB)
#ifdef NOS_MUSL
#define PTHREAD_LABEL(name) "musl_" name
#else
#define PTHREAD_LABEL(name) "newlib_" name
#endif
#ifdef NOS_MUSL
static unsigned signed_futex = 0x80000000u;
static int signed_futex_ready;
static void *signed_futex_worker(void *unused)
{
    (void)unused;
    __atomic_store_n(&signed_futex_ready, 1, __ATOMIC_RELEASE);
    long result = syscall(SYS_futex, &signed_futex, (long)128, (long)(int)0x80000000u,
        (void *)0, (void *)0, (long)0);
    return (void *)(uintptr_t)(result == 0);
}
static unsigned requeue_source = 7, requeue_target = 11;
static int requeue_ready;
static void *requeue_worker(void *argument)
{
    __atomic_add_fetch(&requeue_ready, 1, __ATOMIC_RELEASE);
    long rc = syscall(SYS_futex, &requeue_source, 128L, 7L, (void *)0, (void *)0, 0L);
    return rc ? (void *)-1 : argument;
}
static pthread_mutex_t broadcast_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t broadcast_condition = PTHREAD_COND_INITIALIZER;
static int broadcast_ready, broadcast_go;
static void *broadcast_worker(void *argument)
{
    if (pthread_mutex_lock(&broadcast_lock)) return (void *)-1;
    broadcast_ready++;
    while (!broadcast_go)
        if (pthread_cond_wait(&broadcast_condition, &broadcast_lock)) return (void *)-1;
    if (pthread_mutex_unlock(&broadcast_lock)) return (void *)-1;
    return argument;
}
static void requeue_regression(void)
{
    pthread_t workers[3];
    int ok = !pthread_create(&workers[0], NULL, requeue_worker, (void *)1) &&
        !pthread_create(&workers[1], NULL, requeue_worker, (void *)2);
    if (!ok) { nos_check(0, "musl_futex_requeue_setup"); nos_syscall(NOS_SYS_FINISH, 1, 0, 0); for (;;) { } }
    while (__atomic_load_n(&requeue_ready, __ATOMIC_ACQUIRE) != 2) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    long mismatched = syscall(SYS_futex, &requeue_source, 132L, 0L, 2L, &requeue_target, 99L);
    ok = mismatched == -1 && errno == EAGAIN;
    long moved = syscall(SYS_futex, &requeue_source, 132L, 0L, 2L, &requeue_target, 7L);
    long old_wake = syscall(SYS_futex, &requeue_source, 129L, 2L, (void *)0, (void *)0, 0L);
    long new_wake = syscall(SYS_futex, &requeue_target, 129L, 2L, (void *)0, (void *)0, 0L);
    void *first, *second;
    ok &= moved == 2 && old_wake == 0 && new_wake == 2;
    ok &= !pthread_join(workers[0], &first) && !pthread_join(workers[1], &second) &&
        first == (void *)1 && second == (void *)2;
    nos_check(ok, "musl_futex_atomic_requeue");
    for (unsigned i = 0; i < 3; i++) {
        if (pthread_create(&workers[i], NULL, broadcast_worker, (void *)(uintptr_t)(i + 1))) {
            nos_check(0, "musl_condvar_three_waiter_setup"); nos_syscall(NOS_SYS_FINISH, 1, 0, 0); for (;;) { }
        }
    }
    for (;;) {
        pthread_mutex_lock(&broadcast_lock);
        if (broadcast_ready == 3) break;
        pthread_mutex_unlock(&broadcast_lock);
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    broadcast_go = 1;
    ok = !pthread_cond_broadcast(&broadcast_condition) && !pthread_mutex_unlock(&broadcast_lock);
    for (unsigned i = 0; i < 3; i++) {
        void *value;
        ok &= !pthread_join(workers[i], &value) && value == (void *)(uintptr_t)(i + 1);
    }
    ok &= !pthread_mutex_destroy(&broadcast_lock) && !pthread_cond_destroy(&broadcast_condition);
    nos_check(ok, "musl_condvar_three_waiter_broadcast");
}
#endif
static pthread_mutex_t pthread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pthread_condition = PTHREAD_COND_INITIALIZER;
static int pthread_ready, pthread_go, pthread_count;
static pthread_key_t pthread_key;
static int pthread_shared_fd, pthread_destructors;
static FILE *pthread_shared_stream;
static char pthread_stream_buffer[256];
static unsigned pthread_cpu_mask;
static void key_destructor(void *value)
{ if (value) __atomic_add_fetch(&pthread_destructors, 1, __ATOMIC_RELAXED); }
static void *pthread_worker(void *argument)
{
    if (pthread_setspecific(pthread_key, argument)) return (void *)-1;
    errno = (int)(uintptr_t)argument;
    if (pthread_mutex_lock(&pthread_lock)) return (void *)-1;
    pthread_ready++;
    pthread_cond_broadcast(&pthread_condition);
    while (!pthread_go)
        if (pthread_cond_wait(&pthread_condition, &pthread_lock)) return (void *)-1;
    pthread_mutex_unlock(&pthread_lock);
    for (unsigned i = 0; i < 64; i++) {
        if (pthread_mutex_lock(&pthread_lock)) return (void *)-1;
        pthread_count++;
        if (nos_write(pthread_shared_fd, "x", 1) != 1) return (void *)-1;
        pthread_mutex_unlock(&pthread_lock);
        unsigned cpu = (unsigned)nos_syscall(NOS_SYS_CPU_QUERY, 0, 0, 0);
        __atomic_fetch_or(&pthread_cpu_mask, 1u << cpu, __ATOMIC_RELAXED);
        char format[16];
        if (snprintf(format, sizeof(format), "%.1f", 1.5) != 3 || !u_streq(format, "1.5") ||
            fprintf(pthread_shared_stream, "%c", (uintptr_t)argument == 31 ? 'A' : 'B') != 1)
            return (void *)-1;
        char *allocation = malloc(96);
        if (!allocation) return (void *)-1;
        allocation[0] = (char)i;
        nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
        if (allocation[0] != (char)i || errno != (int)(uintptr_t)argument ||
            pthread_getspecific(pthread_key) != argument) return (void *)-1;
        free(allocation);
    }
    return (void *)((uintptr_t)argument + 1);
}
#endif
static void check_setup(int condition, const char *name)
{
    nos_check(condition, name);
    if (!condition) {
        nos_syscall(NOS_SYS_FINISH, 1, 0, 0);
        for (;;) { }
    }
}
struct pipe_transfer {
    int fd, write, go;
    size_t count;
    intptr_t result;
    char buffer[64];
};
static void *pipe_transfer(void *argument)
{
    struct pipe_transfer *transfer = argument;
    while (!__atomic_load_n(&transfer->go, __ATOMIC_ACQUIRE)) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    transfer->result = transfer->write ? nos_write(transfer->fd, transfer->buffer, transfer->count) :
        nos_read(transfer->fd, transfer->buffer, transfer->count);
    return (void *)(uintptr_t)(transfer->result == (intptr_t)transfer->count);
}
static void pipe_regression(void)
{
    int descriptors[2];
    struct pipe_transfer transfer = { .write = 1, .count = 4, .buffer = "pipe" };
#ifdef NOS_MUSL
    pthread_t worker;
#else
    struct nos_thread worker;
#endif
    check_setup(!nos_pipe(descriptors), "vfs_pipe_blocking_setup");
    transfer.fd = descriptors[1];
#ifdef NOS_MUSL
    check_setup(!pthread_create(&worker, NULL, pipe_transfer, &transfer), "vfs_pipe_reader_worker");
#else
    check_setup(!nos_thread_create(&worker, pipe_transfer, &transfer), "vfs_pipe_reader_worker");
#endif
    intptr_t started = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    char data[256];
    __atomic_store_n(&transfer.go, 1, __ATOMIC_RELEASE);
    intptr_t count = nos_read(descriptors[0], data, 4);
    intptr_t elapsed = nos_syscall(NOS_SYS_TICKS, 0, 0, 0) - started;
    void *result;
#ifdef NOS_MUSL
    int joined = !pthread_join(worker, &result);
#else
    int joined = !nos_thread_join(&worker, &result);
#endif
    nos_check(count == 4 && elapsed >= 2 && joined && result == (void *)1 &&
        data[0] == 'p' && data[3] == 'e', "vfs_pipe_blocking_read");
    nos_close(descriptors[0]); nos_close(descriptors[1]);

    check_setup(!nos_pipe(descriptors), "vfs_pipe_full_setup");
    u_memset(data, 'x', sizeof(data));
    check_setup(nos_write(descriptors[1], data, sizeof(data)) == sizeof(data), "vfs_pipe_fill");
    transfer.fd = descriptors[0]; transfer.write = 0; transfer.count = 64; transfer.go = 0;
#ifdef NOS_MUSL
    check_setup(!pthread_create(&worker, NULL, pipe_transfer, &transfer), "vfs_pipe_writer_worker");
#else
    check_setup(!nos_thread_create(&worker, pipe_transfer, &transfer), "vfs_pipe_writer_worker");
#endif
    started = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    __atomic_store_n(&transfer.go, 1, __ATOMIC_RELEASE);
    count = nos_write(descriptors[1], "y", 1);
    elapsed = nos_syscall(NOS_SYS_TICKS, 0, 0, 0) - started;
#ifdef NOS_MUSL
    joined = !pthread_join(worker, &result);
#else
    joined = !nos_thread_join(&worker, &result);
#endif
    nos_check(count == 1 && elapsed >= 2 && joined && result == (void *)1 &&
        transfer.buffer[0] == 'x', "vfs_pipe_blocking_write");
    nos_close(descriptors[0]); nos_close(descriptors[1]);

    check_setup(!nos_pipe2(descriptors, NOS_O_NONBLOCK), "vfs_pipe_nonblock_setup");
    int flags = nos_getflags(descriptors[0]);
    int ok = (flags & NOS_O_NONBLOCK) && nos_read(descriptors[0], data, 1) == -NOS_EAGAIN;
    ok &= nos_write(descriptors[1], data, sizeof(data)) == sizeof(data) &&
        nos_write(descriptors[1], data, 1) == -NOS_EAGAIN;
    int duplicate = nos_dup_min(descriptors[0], 8);
    ok &= duplicate >= 8 && !nos_setflags(duplicate, 0) && !(nos_getflags(descriptors[0]) & NOS_O_NONBLOCK);
    nos_check(ok, "vfs_pipe_nonblocking");
    nos_close(duplicate); nos_close(descriptors[0]); nos_close(descriptors[1]);

    check_setup(!nos_pipe(descriptors), "vfs_pipe_cancel_setup");
    struct nos_message request = { .op = NOS_VFS_READ, .arg = descriptors[0], .len = 1 }, reply;
    struct nos_call_args call = { .endpoint = 0, .request = (uintptr_t)&request,
        .reply = (uintptr_t)&reply, .timeout_ticks = 2 };
    intptr_t rc = nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&call, 0, 0);
    ok = rc == -NOS_ETIMEDOUT && !nos_close(descriptors[0]);
    ok &= nos_write(descriptors[1], "z", 1) == -NOS_EPIPE;
    ok &= !nos_close(descriptors[1]);
    nos_check(ok, "vfs_pipe_timeout_cancel_reclaims");
}
void nos_libc_regression(void)
{
    struct nos_time_value boundary;
    nos_time_from_ticks(UINT64_C(0x100000001), &boundary);
    int time_ok = boundary.seconds == 42949672 && boundary.nanoseconds == 970000000;
    nos_time_from_ticks(UINT64_C(0x80000000), &boundary);
    nos_check(time_ok && boundary.seconds == 21474836 && boundary.nanoseconds == 480000000,
        "libc_clock_64bit_conversion");
    char byte;
    int fd = nos_open("/safeio", NOS_O_CREAT | NOS_O_RDWR);
    nos_check(fd >= 3, "safe_io_open");
    nos_check(nos_write(fd, (void *)1, 1) == -NOS_EFAULT &&
        nos_read(fd, (void *)1, 1) == -NOS_EFAULT &&
        nos_open((void *)1, NOS_O_RDONLY) == -NOS_EFAULT &&
        nos_stat_fd(fd, (void *)1) == -NOS_EFAULT, "safe_io_bad_pointers");
    nos_check(nos_read(-1, &byte, 0) == -NOS_EBADF &&
        nos_write(-1, &byte, 0) == -NOS_EBADF, "zero_io_bad_descriptor");
    char long_output[1100];
    u_memset(long_output, '.', sizeof(long_output));
    long_output[sizeof(long_output) - 1] = '\n';
    nos_check(nos_write(1, long_output, sizeof(long_output)) == (intptr_t)sizeof(long_output),
        "stdio_long_write");
    nos_close(fd); nos_unlink("/safeio");
    pipe_regression();
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
    posix_directory_regression();
    posix_poll_regression();
    posix_metadata_regression();
#endif

#ifndef NOS_MUSL
    struct nos_thread one, two;
    native_ready = native_go = native_count = 0;
    native_lock.word = native_condition.sequence = 0;
    check_setup(!nos_thread_create(&one, native_worker, (void *)31) &&
        !nos_thread_create(&two, native_worker, (void *)47), "shared_thread_create");
    check_setup(!nos_lock_acquire(&native_lock), "shared_thread_lock");
    while (native_ready != 2)
        check_setup(!nos_condition_wait(&native_condition, &native_lock), "shared_thread_condition_wait");
    native_go = 1; nos_condition_signal(&native_condition, 1); nos_lock_release(&native_lock);
    void *first, *second;
    int shared_ok = !nos_thread_join(&one, &first) && !nos_thread_join(&two, &second) &&
        first == (void *)32 && second == (void *)48 && native_count == 128;
    nos_check(shared_ok, "shared_thread_join_futex_condition");
#ifdef NOS_NEWLIB
    nos_check(shared_ok, "newlib_shared_allocator_errno");
#endif
#else
    /* Native threads without a musl pthread descriptor are intentionally not
     * used in a musl process: errno/stdio require musl's TLS setup. */
    intptr_t mapping = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PAGE_SIZE, 0);
    nos_check(mapping > 0 && munmap((void *)(uintptr_t)mapping, 0) == -1 && errno == EINVAL &&
        mprotect((void *)(uintptr_t)mapping, 0, PROT_READ) == -1 && errno == EINVAL,
        "musl_zero_vm_length");
    *(volatile unsigned *)(uintptr_t)mapping = 123;
    nos_check(!munmap((void *)(uintptr_t)mapping, NOS_PAGE_SIZE), "musl_zero_vm_preserves_mapping");
#endif
#if defined(NOS_MUSL) || defined(NOS_NEWLIB)
#ifdef NOS_MUSL
    pthread_t signed_worker;
    check_setup(!pthread_create(&signed_worker, NULL, signed_futex_worker, NULL), "musl_signed_futex_setup");
    while (!__atomic_load_n(&signed_futex_ready, __ATOMIC_ACQUIRE)) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    long woken = syscall(SYS_futex, &signed_futex, (long)129, (long)1, (void *)0, (void *)0, (long)0);
    void *signed_result;
    check_setup(woken == 1 && !pthread_join(signed_worker, &signed_result) && signed_result == (void *)1,
        "musl_signed_futex_wait_wake");
#endif
#ifdef NOS_MUSL
    requeue_regression();
#endif
    pthread_t pthread_one, pthread_two;
    pthread_shared_fd = nos_open("/pthread-shared", NOS_O_CREAT | NOS_O_RDWR | NOS_O_TRUNC);
    pthread_shared_stream = fopen("/pthread-stdio", "w+");
    check_setup(pthread_shared_stream && !setvbuf(pthread_shared_stream,
        pthread_stream_buffer, _IOFBF, sizeof(pthread_stream_buffer)), PTHREAD_LABEL("pthread_shared_stream_open"));
    pthread_cpu_mask = 0;
    pthread_destructors = 0;
    pthread_ready = pthread_go = pthread_count = 0;
    check_setup(pthread_shared_fd >= 3 && !pthread_key_create(&pthread_key, key_destructor) &&
        !pthread_create(&pthread_one, NULL, pthread_worker, (void *)31) &&
        !pthread_create(&pthread_two, NULL, pthread_worker, (void *)47), PTHREAD_LABEL("pthread_default_create"));
    check_setup(!pthread_mutex_lock(&pthread_lock), PTHREAD_LABEL("pthread_mutex"));
    while (pthread_ready != 2)
        check_setup(!pthread_cond_wait(&pthread_condition, &pthread_lock), PTHREAD_LABEL("pthread_condition_wait"));
    pthread_go = 1; pthread_cond_broadcast(&pthread_condition); pthread_mutex_unlock(&pthread_lock);
    void *pthread_first, *pthread_second;
    nos_check(!pthread_join(pthread_one, &pthread_first) && !pthread_join(pthread_two, &pthread_second) &&
        pthread_first == (void *)32 && pthread_second == (void *)48 && pthread_count == 128 &&
        pthread_destructors == 2, PTHREAD_LABEL("pthread_join_condition_tls_malloc"));
    struct nos_stat shared_stat;
    nos_check(!nos_stat_fd(pthread_shared_fd, &shared_stat) && shared_stat.size == 128 &&
        !nos_close(pthread_shared_fd) && !nos_unlink("/pthread-shared"), PTHREAD_LABEL("pthread_shared_descriptors"));
    char stream_data[128];
    unsigned count_a = 0, count_b = 0;
    int stream_ok = !fflush(pthread_shared_stream) && !fseek(pthread_shared_stream, 0, SEEK_SET) &&
        fread(stream_data, 1, sizeof(stream_data), pthread_shared_stream) == sizeof(stream_data);
    for (unsigned i = 0; i < sizeof(stream_data); i++) {
        count_a += stream_data[i] == 'A'; count_b += stream_data[i] == 'B';
    }
    stream_ok &= !fclose(pthread_shared_stream) && !nos_unlink("/pthread-stdio");
    nos_check(stream_ok && count_a == 64 && count_b == 64, PTHREAD_LABEL("pthread_shared_stdio"));
    unsigned cpus = (unsigned)nos_syscall(NOS_SYS_CPU_QUERY, 1, 0, 0);
    if (cpus > 1) nos_check((pthread_cpu_mask & (pthread_cpu_mask - 1)) != 0,
        PTHREAD_LABEL("pthread_cross_cpu_execution"));
    pthread_key_delete(pthread_key);
    nos_check(pthread_cancel(pthread_self()) == ENOTSUP, PTHREAD_LABEL("cancel_explicit_unsupported"));
#endif
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
    struct timespec before, after, delay = { 0, 10000000 };
    nos_check(!clock_gettime(CLOCK_MONOTONIC, &before) && !nanosleep(&delay, NULL) &&
        !clock_gettime(CLOCK_MONOTONIC, &after) &&
        (after.tv_sec > before.tv_sec || after.tv_nsec > before.tv_nsec), "libc_monotonic_sleep");
    struct timespec deadline = after;
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    nos_check(!clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) &&
        !clock_gettime(CLOCK_MONOTONIC, &after) &&
        (after.tv_sec > deadline.tv_sec || (after.tv_sec == deadline.tv_sec && after.tv_nsec >= deadline.tv_nsec)),
        "libc_monotonic_absolute_sleep");
#ifdef NOS_ARM_M
    errno = 0;
    nos_check(clock_gettime(CLOCK_REALTIME, &after) == -1 && errno == ENOTSUP,
        "libc_realtime_unsupported");
#else
    struct timeval wall;
    struct timespec resolution;
    int rtc_ok = !clock_gettime(CLOCK_REALTIME, &after) && !gettimeofday(&wall, NULL) &&
        !clock_getres(CLOCK_REALTIME, &resolution) && after.tv_sec >= 1577836800 &&
        wall.tv_sec >= after.tv_sec && wall.tv_sec - after.tv_sec <= 1 &&
        wall.tv_usec >= 0 && wall.tv_usec < 1000000 && resolution.tv_sec >= 0 &&
        resolution.tv_nsec >= 0 && resolution.tv_nsec < 1000000000;
    nos_check(rtc_ok, "libc_realtime_clock_gettimeofday");
    if (rtc_ok) {
        deadline = after;
        deadline.tv_nsec += 20000000;
        if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
        rtc_ok = !clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &deadline, NULL) &&
            !clock_gettime(CLOCK_REALTIME, &after) && (after.tv_sec > deadline.tv_sec ||
            (after.tv_sec == deadline.tv_sec && after.tv_nsec >= deadline.tv_nsec));
    }
    nos_check(rtc_ok, "libc_realtime_absolute_sleep");
#endif
#endif
}
