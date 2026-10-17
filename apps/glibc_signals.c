/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
static volatile sig_atomic_t received, queued_value, used_altstack;
static volatile sig_atomic_t alarms;
static uintptr_t alternate_begin, alternate_end;
static void handler(int signal, siginfo_t *info, void *context)
{
    volatile unsigned char local = 0;
    (void)context;
    if (signal == SIGUSR1) received++;
    if (signal == SIGALRM) alarms++;
    if (info->si_code == SI_QUEUE) queued_value = info->si_value.sival_int;
    uintptr_t address = (uintptr_t)&local;
    if (address >= alternate_begin && address < alternate_end) used_altstack = 1;
}
static int check(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL %s errno=%d received=%d\n", name, errno, received); return 0; }
    printf("PASS %s\n", name); return 1;
}
int main(void)
{
    struct sigaction action; memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler; action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (!check(sigaction(SIGUSR1, &action, NULL) == 0 && raise(SIGUSR1) == 0 && received == 1,
               "glibc_signal_handler_sigreturn")) return 100;
    sigset_t set, original, pending; sigemptyset(&set); sigaddset(&set, SIGUSR1);
    union sigval value; value.sival_int = 173;
    if (sigprocmask(SIG_BLOCK, &set, &original) || sigqueue(getpid(), SIGUSR1, value) || sigpending(&pending)) return 101;
    if (!check(sigismember(&pending, SIGUSR1) == 1 && received == 1 &&
               sigprocmask(SIG_SETMASK, &original, NULL) == 0 && received == 2 && queued_value == 173,
               "glibc_signal_mask_pending_queue")) return 102;
    stack_t alternate; memset(&alternate, 0, sizeof(alternate));
    alternate.ss_size = 32768; alternate.ss_sp = malloc(alternate.ss_size);
    if (!alternate.ss_sp) return 103;
    alternate_begin = (uintptr_t)alternate.ss_sp; alternate_end = alternate_begin + alternate.ss_size;
    action.sa_flags |= SA_ONSTACK;
    if (!check(sigaltstack(&alternate, NULL) == 0 && sigaction(SIGUSR1, &action, NULL) == 0 &&
               raise(SIGUSR1) == 0 && received == 3 && used_altstack,
               "glibc_signal_alternate_stack")) return 104;
    stack_t disable; memset(&disable, 0, sizeof(disable)); disable.ss_flags = SS_DISABLE;
    if (sigaltstack(&disable, NULL)) return 105;
    free(alternate.ss_sp);
    sigemptyset(&set); sigaddset(&set, SIGRTMIN);
    if (sigprocmask(SIG_BLOCK, &set, &original)) return 106;
    value.sival_int = 41; if (sigqueue(getpid(), SIGRTMIN, value)) return 107;
    value.sival_int = 42; if (sigqueue(getpid(), SIGRTMIN, value)) return 108;
    siginfo_t info; struct timespec zero = {0, 0};
    int first = sigtimedwait(&set, &info, &zero), first_value = info.si_value.sival_int;
    int second = sigtimedwait(&set, &info, &zero), second_value = info.si_value.sival_int;
    errno = 0; int empty = sigtimedwait(&set, &info, &zero);
    if (!check(first == SIGRTMIN && second == SIGRTMIN && first_value == 41 && second_value == 42 &&
               empty == -1 && errno == EAGAIN && sigprocmask(SIG_SETMASK, &original, NULL) == 0,
               "glibc_signal_realtime_fifo_wait")) return 109;
    memset(&action, 0, sizeof(action)); sigemptyset(&action.sa_mask); action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return 110;
    int pipefd[2]; if (pipe(pipefd) || close(pipefd[0])) return 111;
    errno = 0; ssize_t result = write(pipefd[1], "x", 1);
    int saved_errno = errno; if (close(pipefd[1])) return 112;
    if (!check(result == -1 && saved_errno == EPIPE, "glibc_signal_pipe_ignore")) return 113;
    action.sa_sigaction = handler; action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGALRM, &action, NULL)) return 114;
    struct itimerval timer; memset(&timer, 0, sizeof(timer)); timer.it_value.tv_usec = 30000;
    struct timespec duration = {1, 0}, remaining;
    if (setitimer(ITIMER_REAL, &timer, NULL)) return 115;
    errno = 0; int sleeping = nanosleep(&duration, &remaining), sleep_error = errno;
    if (!check(sleeping == -1 && sleep_error == EINTR && alarms == 1 &&
               getitimer(ITIMER_REAL, &timer) == 0 && !timer.it_value.tv_sec && !timer.it_value.tv_usec,
               "glibc_signal_realtime_interval_timer")) return 116;
    puts("PASS glibc_posix_signals"); return 0;
}
