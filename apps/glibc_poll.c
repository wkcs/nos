/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int failed(const char *operation)
{ fprintf(stderr, "FAIL glibc_poll_select (%s, errno=%d)\n", operation, errno); return 91; }
static long long milliseconds(const struct timespec *value)
{ return (long long)value->tv_sec * 1000 + value->tv_nsec / 1000000; }
static volatile sig_atomic_t caught;
static void catch_signal(int number) { (void)number; caught++; }
static int temporary_mask_wait(void)
{
    struct sigaction action = {0};
    action.sa_handler = catch_signal;
    sigemptyset(&action.sa_mask);
    sigset_t blocked, original, empty, current;
    sigemptyset(&blocked); sigaddset(&blocked, SIGUSR1); sigemptyset(&empty);
    if (sigaction(SIGUSR1, &action, NULL) || sigprocmask(SIG_BLOCK, &blocked, &original))
        return failed("mask_setup");
    pid_t parent = getpid();
    /* The sender may run before or during the wait. Both cases must deliver
     * under the temporary mask, interrupt the wait, and restore SIGUSR1. */
    for (int kind = 0; kind < 2; kind++) {
        for (int repeat = 0; repeat < 4; repeat++) {
            sig_atomic_t before = caught;
            pid_t child = fork();
            if (!child) _exit(kill(parent, SIGUSR1) ? 1 : 0);
            if (child < 0) return failed("mask_fork");
            struct timespec deadline = {1, 0};
            errno = 0;
            int result = kind ? pselect(0, NULL, NULL, NULL, &deadline, &empty) :
                                ppoll(NULL, 0, &deadline, &empty);
            int error = errno, status;
            if (result != -1 || error != EINTR || caught != before + 1 ||
                deadline.tv_sec != 1 || deadline.tv_nsec ||
                sigprocmask(SIG_SETMASK, NULL, &current) || sigismember(&current, SIGUSR1) != 1 ||
                waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
                return failed(kind ? "pselect_atomic_mask" : "ppoll_atomic_mask");
        }
    }
    if (sigprocmask(SIG_SETMASK, &original, NULL)) return failed("mask_restore");
    puts("PASS glibc_ppoll_pselect_atomic_sigmask");
    return 0;
}
int main(void)
{
    int descriptors[2];
    if (pipe2(descriptors, O_NONBLOCK | O_CLOEXEC)) return failed("pipe2");
    if (fcntl(descriptors[0], F_GETFD) != FD_CLOEXEC || fcntl(descriptors[1], F_GETFD) != FD_CLOEXEC)
        return failed("cloexec");
    int copied = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 5);
    if (copied < 5 || fcntl(copied, F_GETFD) != FD_CLOEXEC || close(copied)) return failed("dup_cloexec");
    struct pollfd query[2] = {{descriptors[0], POLLIN, -1}, {descriptors[1], POLLOUT, -1}};
    if (poll(query, 2, 0) != 1 || query[0].revents || !(query[1].revents & POLLOUT)) return failed("empty");
    if (write(descriptors[1], "abc", 3) != 3 || poll(query, 2, 0) != 2 || !(query[0].revents & POLLIN))
        return failed("readable");
    fd_set input, output;
    FD_ZERO(&input); FD_ZERO(&output);
    FD_SET(descriptors[0], &input); FD_SET(descriptors[1], &output);
    struct timeval timeout = {0, 0};
    if (select(descriptors[1] + 1, &input, &output, NULL, &timeout) != 2 ||
        !FD_ISSET(descriptors[0], &input) || !FD_ISSET(descriptors[1], &output)) return failed("select");
    char bytes[4] = {0};
    if (read(descriptors[0], bytes, 3) != 3 || bytes[0] != 'a' || bytes[2] != 'c') return failed("no_consume");
    puts("PASS glibc_poll_select_readiness");
    struct timespec before, after;
    if (clock_gettime(CLOCK_MONOTONIC, &before) || poll(query, 1, 25) || clock_gettime(CLOCK_MONOTONIC, &after) ||
        milliseconds(&after) - milliseconds(&before) < 20) return failed("timeout");
    if (close(descriptors[1]) || poll(query, 1, 0) != 1 || !(query[0].revents & POLLHUP) ||
        read(descriptors[0], bytes, sizeof(bytes)) != 0) return failed("eof");
    struct pollfd invalid[2] = {{32767, POLLIN, 0}, {-1, POLLIN, -1}};
    if (poll(invalid, 2, 0) != 1 || !(invalid[0].revents & POLLNVAL) || invalid[1].revents) return failed("invalid");
    FD_ZERO(&input); FD_SET(31, &input); timeout = (struct timeval){0, 0}; errno = 0;
    if (select(32, &input, NULL, NULL, &timeout) != -1 || errno != EBADF) return failed("select_invalid");
    if (close(descriptors[0])) return failed("close");
    puts("PASS glibc_poll_timeout_eof_invalid");
    return temporary_mask_wait();
}
