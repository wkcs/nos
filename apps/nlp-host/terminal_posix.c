/* Copyright (C) 2026 NOS contributors. Linux host PTY fixture, never enabled implicitly. */
#define _GNU_SOURCE
#include "terminal_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
static int open_terminal(void *context, uint16_t cols, uint16_t rows, const char *term)
{
    struct nlp_posix_terminal *t = context;
    struct winsize size = {.ws_row = rows, .ws_col = cols};
    if (t->pid > 0)
        return -NLP_BUSY;
    pid_t parent = getpid();
    pid_t pid = forkpty(&t->master, NULL, NULL, &size);
    if (pid < 0)
        return -NLP_NO_SPACE;
    if (!pid) {
#ifdef __linux__
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
            _exit(127);
#else
        (void)parent;
#endif
        closefrom(3);
        setenv("TERM", *term ? term : "dumb", 1);
        setenv("PS1", "nlp$ ", 1);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }
    fcntl(t->master, F_SETFL, O_NONBLOCK);
    fcntl(t->master, F_SETFD, FD_CLOEXEC);
    t->pid = pid;
    t->input_size = t->input_at = 0;
    t->exited = 0;
    t->status = 0;
    return 0;
}
static int resize_terminal(void *context, uint16_t cols, uint16_t rows)
{
    struct nlp_posix_terminal *t = context;
    struct winsize size = {.ws_row = rows, .ws_col = cols};
    return t->master >= 0 && !ioctl(t->master, TIOCSWINSZ, &size) ? 0 : -NLP_DISCONNECTED;
}
static int interrupt_terminal(void *context, unsigned signal)
{
    struct nlp_posix_terminal *t = context;
    if (t->pid <= 0)
        return -NLP_DISCONNECTED;
    return kill(-t->pid, signal == 2 ? SIGINT : SIGTERM) ? -NLP_DISCONNECTED : 0;
}
static int write_terminal(void *context, const uint8_t *bytes, size_t size)
{
    struct nlp_posix_terminal *t = context;
    if (t->master < 0)
        return -NLP_DISCONNECTED;
    if (size > sizeof(t->input))
        return -NLP_RANGE;
    if (t->input_size)
        return -NLP_BUSY;
    if (size)
        memcpy(t->input, bytes, size);
    t->input_size = size;
    t->input_at = 0;
    return 0;
}
static int read_terminal(void *context, uint8_t *bytes, size_t size, size_t *read_size)
{
    struct nlp_posix_terminal *t = context;
    if (t->master < 0)
        return -NLP_DISCONNECTED;
    ssize_t count = read(t->master, bytes, size);
    if (count > 0) {
        *read_size = (size_t)count;
        return 0;
    }
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return -NLP_BUSY;
    return -NLP_DISCONNECTED;
}
static int exit_terminal(void *context, int32_t *status)
{
    struct nlp_posix_terminal *t = context;
    nlp_posix_terminal_poll(t);
    if (!t->exited)
        return -NLP_BUSY;
    *status = t->status;
    return 0;
}
static void close_terminal(void *context)
{
    struct nlp_posix_terminal *t = context;
    if (t->master >= 0)
        close(t->master);
    t->master = -1;
    t->input_size = t->input_at = 0;
    if (t->pid > 0)
        (void)kill(-t->pid, SIGKILL);
    nlp_posix_terminal_poll(t);
}
void nlp_posix_terminal_init(struct nlp_posix_terminal *t, struct nlp_terminal_ops *ops)
{
    memset(t, 0, sizeof(*t));
    t->master = -1;
    *ops = (struct nlp_terminal_ops){t,
                                     open_terminal,
                                     resize_terminal,
                                     interrupt_terminal,
                                     write_terminal,
                                     read_terminal,
                                     exit_terminal,
                                     close_terminal};
}
void nlp_posix_terminal_poll(struct nlp_posix_terminal *t)
{
    if (t->pid > 0) {
        int status;
        pid_t result = waitpid(t->pid, &status, WNOHANG);
        if (result == t->pid) {
            t->pid = 0;
            t->exited = 1;
            t->status = WIFEXITED(status)     ? WEXITSTATUS(status)
                        : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                              : 1;
        }
    }
    if (t->input_size && t->master >= 0) {
        ssize_t count = write(t->master, t->input + t->input_at, t->input_size - t->input_at);
        if (count > 0) {
            t->input_at += (size_t)count;
            if (t->input_at == t->input_size)
                t->input_size = t->input_at = 0;
        }
    }
}
