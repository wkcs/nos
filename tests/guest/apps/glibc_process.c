/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/utsname.h>
static volatile sig_atomic_t signals;
static int private_word = 17;
static volatile uintptr_t invalid_address = 1;
static void count_signal(int signal) { (void)signal; signals++; }
static void *thread_exec(void *argument)
{
    extern char **environ;
    char **arguments = argument; execve(arguments[0], arguments, environ); _exit(128);
}
static int report(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL %s errno=%d\n", name, errno); return 0; }
    printf("PASS %s\n", name); return 1;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "exec-child")) {
        int retained = atoi(argv[2]), closed = atoi(argv[3]);
        pid_t expected = (pid_t)atoi(argv[4]); char c;
        if (getpid() != expected || read(retained, &c, 1) != 1 || c != 'b') return 125;
        errno = 0; if (fcntl(closed, F_GETFD) != -1 || errno != EBADF) return 126;
        return 37;
    }
    struct timeval wall; struct utsname identity; time_t seconds = time(NULL);
    if (!report(gettimeofday(&wall, NULL) == 0 && seconds > 1700000000 &&
                wall.tv_sec >= seconds && wall.tv_sec <= seconds + 1 && wall.tv_usec >= 0 && wall.tv_usec < 1000000 &&
                uname(&identity) == 0 && !strcmp(identity.sysname, "NOS") && !strcmp(identity.release, "microkernel") &&
                !strcmp(identity.nodename, "nos"), "glibc_time_gettimeofday_uname")) return 119;
    int fd = open("/fork-description", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || write(fd, "abc", 3) != 3 || lseek(fd, 0, SEEK_SET)) return 120;
    pid_t original = getpid(), child = fork();
    if (!child) {
        char c; private_word = 91;
        _exit(getpid() != original && read(fd, &c, 1) == 1 && c == 'a' ? 23 : 121);
    }
    int status;
    if (!report(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23 &&
                private_word == 17 && lseek(fd, 0, SEEK_CUR) == 1, "glibc_fork_private_memory_shared_ofd")) return 121;
    struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = count_signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) return 122;
    child = fork();
    if (!child) { struct timespec short_sleep = {0, 30000000}; nanosleep(&short_sleep, NULL); kill(original, SIGUSR1); _exit(0); }
    struct timespec long_sleep = {1, 0}, remain;
    errno = 0; int slept = nanosleep(&long_sleep, &remain), error = errno;
    if (!report(child > 0 && slept == -1 && error == EINTR && signals == 1 && remain.tv_sec >= 0 &&
                (remain.tv_sec || remain.tv_nsec) && waitpid(child, &status, 0) == child,
                "glibc_fork_signal_interrupt_nanosleep")) return 123;
    child = fork();
    if (!child) { struct timespec duration = {0, 50000000}; nanosleep(&duration, NULL); _exit(29); }
    int nonblocking = waitpid(child, &status, WNOHANG);
    if (!report(child > 0 && nonblocking == 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 29,
                "glibc_waitpid_nonblocking_status")) return 124;
    extern char **environ;
    errno = 0; int failed = execve("/exec-does-not-exist", argv, environ), missing_error = errno;
    errno = 0; int bad = execve((const char *)invalid_address, argv, environ), bad_error = errno;
    if (!report(failed == -1 && missing_error == ENOENT && bad == -1 && bad_error == EFAULT &&
                getpid() == original && private_word == 17 && lseek(fd, 0, SEEK_CUR) == 1,
                "glibc_execve_failed_preserves_image")) return 125;
    int restart_pipe[2]; if (pipe(restart_pipe)) return 126;
    action.sa_flags = SA_RESTART; if (sigaction(SIGUSR1, &action, NULL)) return 126;
    child = fork();
    if (!child) {
        close(restart_pipe[0]); struct timespec delay = {0, 30000000};
        nanosleep(&delay, NULL); kill(original, SIGUSR1); nanosleep(&delay, NULL);
        _exit(write(restart_pipe[1], "x", 1) == 1 ? 0 : 126);
    }
    close(restart_pipe[1]); char byte = 0;
    ssize_t restarted = read(restart_pipe[0], &byte, 1); close(restart_pipe[0]);
    if (!report(child > 0 && restarted == 1 && byte == 'x' && signals == 2 &&
                waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
                "glibc_signal_restart_blocking_read")) return 126;
    child = fork();
    if (!child) { raise(SIGTERM); _exit(99); }
    if (!report(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
                "glibc_signal_default_termination_status")) return 126;
    child = fork();
    if (!child) {
        pid_t self = getpid(); int before = signals;
        pid_t session = setsid(), sid = getsid(0), group = getpgrp();
        int ok = session == self && sid == self && group == self;
        errno = 0; int changed = setpgid(0, 0), change_error = errno;
        ok &= changed == -1 && change_error == EPERM;
        int sent = kill(0, SIGUSR1); ok &= sent == 0 && signals == before + 1;
        if (!ok) fprintf(stderr, "session diagnostic pid=%d sid=%d/%d group=%d setpgid=%d/%d kill=%d signals=%d/%d\n",
                         self, session, sid, group, changed, change_error, sent, signals, before);
        _exit(ok ? 0 : 131);
    }
    if (!report(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
                "glibc_process_session_group_signals")) return 131;
    int closed = dup(fd); if (closed < 0 || fcntl(closed, F_SETFD, FD_CLOEXEC)) return 125;
    child = fork();
    if (!child) {
        char keep[16], gone[16], pid[16];
        snprintf(keep, sizeof(keep), "%d", fd); snprintf(gone, sizeof(gone), "%d", closed); snprintf(pid, sizeof(pid), "%d", getpid());
        char *arguments[] = {argv[0], "exec-child", keep, gone, pid, NULL};
        execve(argv[0], arguments, environ); _exit(127);
    }
    if (!report(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37,
                "glibc_execve_pid_fd_cloexec")) return 127;
    if (lseek(fd, 1, SEEK_SET) != 1) return 128;
    child = fork();
    if (!child) {
        char keep[16], gone[16], pid[16];
        snprintf(keep, sizeof(keep), "%d", fd); snprintf(gone, sizeof(gone), "%d", closed); snprintf(pid, sizeof(pid), "%d", getpid());
        char *arguments[] = {argv[0], "exec-child", keep, gone, pid, NULL};
        pthread_t worker; if (pthread_create(&worker, NULL, thread_exec, arguments)) _exit(129);
        pthread_join(worker, NULL); _exit(130);
    }
    if (!report(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37,
                "glibc_execve_nonleader_thread")) return 129;
    const char *alias = "/tmp/process-exec-alias";
    if (lseek(fd, 1, SEEK_SET) != 1 || symlink("/bin/glibc_process", alias)) return 132;
    child = fork();
    if (!child) {
        char keep[16], gone[16], pid[16];
        snprintf(keep, sizeof(keep), "%d", fd); snprintf(gone, sizeof(gone), "%d", closed); snprintf(pid, sizeof(pid), "%d", getpid());
        char *arguments[] = {"./process-exec-alias", "exec-child", keep, gone, pid, NULL};
        if (chdir("/tmp")) _exit(132);
        execve(arguments[0], arguments, environ); _exit(133);
    }
    int alias_ok = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37;
    unlink(alias);
    if (!report(alias_ok, "glibc_execve_relative_symlink")) return 133;
    close(closed); close(fd); unlink("/fork-description"); puts("PASS glibc_posix_process"); return 0;
}
