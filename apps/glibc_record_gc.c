/* Copyright (C) 2026 NOS contributors. Used by the native lifecycle supervisor. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Private fixture layout, shared with record_lock_gc_regression.h. */
struct pipe_gc_control {
    uint32_t armed, observer_pid, observer_ready, start_observer;
    uint32_t done, write_returned, capacity, result;
};
_Static_assert(sizeof(struct pipe_gc_control) == 32, "pipe GC fixture layout");
static int pipe_gc_reader(int fd, int control_fd, struct pipe_gc_control *control)
{
    struct timespec delay = {0, 10000000};
    while (!__atomic_load_n(&control->start_observer, __ATOMIC_ACQUIRE)) {
        if (nanosleep(&delay, NULL)) _exit(134);
    }
    unsigned total = 0, result = 0;
    while (total < control->capacity) {
        unsigned char buffer[64];
        size_t count = control->capacity - total;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        struct pollfd input = {fd, POLLIN, 0};
        if (poll(&input, 1, 3000) != 1) { result = 135; break; }
        ssize_t got = read(fd, buffer, count);
        if (got <= 0 || (size_t)got > count) { result = 136; break; }
        for (ssize_t i = 0; i < got; i++) if (buffer[i] != 0x6b) result = 137;
        total += (unsigned)got;
    }
    if (!result) {
        struct pollfd input = {fd, POLLIN, 0};
        unsigned char extra;
        /* No canceled write byte may be committed, and no pending OFD pin
         * may keep the final writer alive after the queued data is drained. */
        if (poll(&input, 1, 3000) != 1 || !(input.revents & POLLHUP) || read(fd, &extra, 1) != 0)
            result = 138;
    }
    if (close(fd) || close(control_fd)) result = 139;
    control->result = result;
    __atomic_store_n(&control->done, 1u, __ATOMIC_RELEASE);
    /* Exit releases this shared mapping. The supervisor waits for GROUP0
     * and the outer test checks the exact physical-page baseline. */
    _exit(result ? 141 : 0);
}
static int pipe_gc_owner(void)
{
    int control_fd = open("/tmp/pipe-gc-control", O_RDWR), descriptors[2];
    if (control_fd < 0) return 126;
    struct pipe_gc_control *control = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, control_fd, 0);
    if (control == MAP_FAILED || pipe(descriptors)) return 127;
    int flags = fcntl(descriptors[1], F_GETFL);
    if (flags < 0 || fcntl(descriptors[1], F_SETFL, flags | O_NONBLOCK)) return 128;
    unsigned char fill[64]; memset(fill, 0x6b, sizeof(fill));
    unsigned total = 0;
    for (;;) {
        ssize_t count = write(descriptors[1], fill, sizeof(fill));
        if (count < 0 && errno == EAGAIN) break;
        if (count <= 0 || count > (ssize_t)sizeof(fill) || total >= 65536) return 129;
        total += (unsigned)count;
    }
    /* A rejected 64-byte atomic write alone does not prove zero free bytes. */
    for (;;) {
        ssize_t count = write(descriptors[1], fill, 1);
        if (count < 0 && errno == EAGAIN) break;
        if (count != 1 || total >= 65536) return 129;
        total++;
    }
    if (!total || fcntl(descriptors[1], F_SETFL, flags)) return 130;
    control->capacity = total;
    pid_t observer = fork();
    if (observer < 0) return 131;
    if (!observer) {
        if (close(descriptors[1])) _exit(132);
        control->observer_pid = (uint32_t)getpid();
        __atomic_store_n(&control->observer_ready, 1u, __ATOMIC_RELEASE);
        return pipe_gc_reader(descriptors[0], control_fd, control);
    }
    if (close(descriptors[0])) return 133;
    /* Priority zero runs until this full-pipe WRITE has actually blocked and
     * its priority donation has driven VFS back to RECV. The single-CPU
     * priority-two supervisor cannot observe armed earlier than that. */
    __atomic_store_n(&control->armed, 1u, __ATOMIC_RELEASE);
    unsigned char forbidden = 0x7f;
    (void)write(descriptors[1], &forbidden, 1);
    __atomic_store_n(&control->write_returned, 1u, __ATOMIC_RELEASE);
    return 142;
}

static int fork_lease(void)
{
    const char *path = "/tmp/fork-fd-lease";
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600), channel[2];
    if (fd < 0 || write(fd, "pin", 3) != 3 || lseek(fd, 0, SEEK_SET) || pipe(channel)) return 118;
    pid_t middle = fork();
    if (middle < 0) return 119;
    if (!middle) {
        pid_t leaf = fork();
        if (leaf < 0) _exit(120);
        if (leaf) _exit(0);
        /* Fork selects the child first. Yield without VFS so its parent can
         * exit while both trusted inheritance notifications remain queued. */
        struct timespec delay = {0, 30000000};
        if (nanosleep(&delay, NULL)) _exit(121);
        char bytes[3];
        if (read(fd, bytes, 3) != 3 || memcmp(bytes, "pin", 3) || write(channel[1], "K", 1) != 1)
            _exit(122);
        _exit(0);
    }
    int status;
    if (waitpid(middle, &status, 0) != middle || !WIFEXITED(status) || WEXITSTATUS(status)) return 123;
    char byte;
    struct pollfd input = {channel[0], POLLIN, 0};
    if (poll(&input, 1, 5000) != 1 || read(channel[0], &byte, 1) != 1 || byte != 'K') return 124;
    if (close(channel[0]) || close(channel[1]) || close(fd) || unlink(path)) return 125;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 111;
    if (!strcmp(argv[1], "pipe-owner")) return pipe_gc_owner();
    if (!strcmp(argv[1], "fork-lease")) return fork_lease();
    int owner = !strcmp(argv[1], "owner");
    if (!owner && strcmp(argv[1], "observer")) return 112;
    const char *path = "/tmp/record-exit-gc";
    int fd = open(path, owner ? O_CREAT | O_EXCL | O_RDWR : O_RDWR, 0600);
    if (fd < 0) return 113;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    if (!owner && (fcntl(fd, F_GETLK, &lock) || lock.l_type != F_UNLCK)) return 114;
    lock.l_type = F_WRLCK;
    if (fcntl(fd, F_SETLK, &lock)) return 115;
    if (owner) {
        /* A second RPC proves the first reply's lock commit has completed.
         * Its reply leaves VFS runnable at lower priority on the single CPU,
         * so exit is queued rather than delivered directly to a blocked RECV.
         * No close/stdio RPC may drain that exit before the supervisor churns. */
        struct stat status;
        if (fstat(fd, &status)) return 116;
        _exit(0);
    }
    if (close(fd) || unlink(path)) return 117;
    return 0;
}
