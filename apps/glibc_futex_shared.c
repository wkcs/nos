/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_PRIVATE 128
int main(void)
{
    const char *path = "/shared-futex-key";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, 8192)) return 140;
    unsigned *parent = mmap((void *)(uintptr_t)0x0b000000, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 4096);
    if (parent == MAP_FAILED) return 141;
    parent[7] = parent[8] = 0;
    int pipefd[2]; if (pipe(pipefd)) return 142;
    pid_t child = fork();
    if (!child) {
        close(pipefd[0]); munmap(parent, 4096); close(fd);
        fd = open(path, O_RDWR);
        unsigned *other = mmap((void *)(uintptr_t)0x0e000000, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 4096);
        if (other == MAP_FAILED || other == parent) _exit(143);
        if (write(pipefd[1], "r", 1) != 1) _exit(144);
        struct timespec timeout = {2, 0};
        int result = syscall(SYS_futex, other + 7, FUTEX_WAIT, 0, &timeout, 0, 0);
        _exit(result == 0 ? 0 : 145);
    }
    close(pipefd[1]); char ready;
    if (child < 0 || read(pipefd[0], &ready, 1) != 1) return 146;
    if (syscall(SYS_futex, parent + 7, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0)) return 147;
    long moved = 0;
    for (unsigned i = 0; i < 50 && !moved; i++) {
        moved = syscall(SYS_futex, parent + 7, FUTEX_REQUEUE, 0, 1, parent + 8, 0);
        if (moved < 0) break;
        if (!moved) { struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL); }
    }
    int source = syscall(SYS_futex, parent + 7, FUTEX_WAKE, 1, 0, 0, 0);
    int target = syscall(SYS_futex, parent + 8, FUTEX_WAKE, 1, 0, 0, 0);
    int status = 0; pid_t waited = waitpid(child, &status, 0);
    int ok = moved == 1 && source == 0 && target == 1 && waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
    close(pipefd[0]); munmap(parent, 4096); close(fd); unlink(path);
    if (!ok) { fprintf(stderr, "FAIL glibc_shared_futex_alias_requeue moved=%ld wake=%d/%d status=%d errno=%d\n", moved, source, target, status, errno); return 148; }
    puts("PASS glibc_shared_futex_alias_requeue"); return 0;
}
