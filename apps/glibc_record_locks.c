/* Copyright (C) 2026 NOS contributors. POSIX process-owned record locks. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char filename[] = "record-lock-file";
static volatile sig_atomic_t received;
static int die(const char *step)
{ fprintf(stderr, "FAIL glibc_record_locks %s errno=%d\n", step, errno); return 95; }
static int transfer(int fd, void *buffer, size_t count, int writing)
{
    size_t done = 0;
    while (done < count) {
        ssize_t n = writing ? write(fd, (char *)buffer + done, count - done) :
            read(fd, (char *)buffer + done, count - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
static int byte_out(int fd, char byte) { return transfer(fd, &byte, 1, 1); }
static int byte_in(int fd, char expected)
{ char byte; return transfer(fd, &byte, 1, 0) || byte != expected ? -1 : 0; }
static int wait_ok(pid_t pid)
{
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
static int record(int fd, int command, short type, short whence, int64_t start, int64_t length, struct flock *output)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type; lock.l_whence = whence; lock.l_start = (off_t)start; lock.l_len = (off_t)length;
    int rc = fcntl(fd, command, &lock);
    if (output) *output = lock;
    return rc;
}
static int unlock_all(int fd) { return record(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 0, NULL); }
static int conflict_error(int error) { return error == EACCES || error == EAGAIN; }
struct request { int command; short type, whence; int64_t start, length; };
struct response { int result, error; int64_t start, length; int32_t pid; short type, whence; };
struct observer { pid_t pid; int command, response; };
static struct observer observer_start(int fd)
{
    int commands[2], replies[2];
    struct observer server = { -1, -1, -1 };
    if (pipe(commands) || pipe(replies)) return server;
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        for (;;) {
            struct request input;
            if (transfer(commands[0], &input, sizeof(input), 0)) _exit(2);
            if (!input.command) _exit(0);
            struct flock lock;
            errno = 0;
            int rc = record(fd, input.command, input.type, input.whence, input.start, input.length, &lock);
            struct response output = { rc, errno, lock.l_start, lock.l_len, lock.l_pid, lock.l_type, lock.l_whence };
            if (transfer(replies[1], &output, sizeof(output), 1)) _exit(3);
        }
    }
    close(commands[0]); close(replies[1]);
    if (child < 0) { close(commands[1]); close(replies[0]); return server; }
    server.pid = child; server.command = commands[1]; server.response = replies[0]; return server;
}
static int observer_call(struct observer *server, int command, short type, short whence,
    int64_t start, int64_t length, struct response *output)
{
    struct request request = { command, type, whence, start, length };
    return transfer(server->command, &request, sizeof(request), 1) ||
        transfer(server->response, output, sizeof(*output), 0) ? -1 : 0;
}
static int observer_get(struct observer *server, int64_t start, int64_t length,
    short type, pid_t owner, int64_t held_start, int64_t held_length)
{
    struct response answer;
    if (observer_call(server, F_GETLK, F_WRLCK, SEEK_SET, start, length, &answer) || answer.result || answer.type != type)
        return -1;
    if (type == F_UNLCK) return 0;
    return answer.pid == owner && answer.whence == SEEK_SET && answer.start == held_start && answer.length == held_length ? 0 : -1;
}
static int observer_stop(struct observer *server)
{
    struct request stop = { 0, 0, 0, 0, 0 };
    int rc = transfer(server->command, &stop, sizeof(stop), 1);
    close(server->command); close(server->response);
    return rc || wait_ok(server->pid) ? -1 : 0;
}
static int ranges_and_ownership(int fd)
{
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 10, 20, NULL)) return die("parent_initial_lock");
    struct observer observer = observer_start(fd);
    if (observer.pid < 0) return die("observer_fork");
    struct response answer;
    if (observer_get(&observer, 0, 100, F_WRLCK, getpid(), 10, 20) ||
        observer_call(&observer, F_SETLK, F_WRLCK, SEEK_SET, 10, 1, &answer) ||
        answer.result != -1 || !conflict_error(answer.error)) return die("fork_locks_not_inherited");
    struct flock own;
    if (record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 100, &own) || own.l_type != F_UNLCK)
        return die("getlk_ignores_own_locks");
    if (unlock_all(fd) || record(fd, F_SETLK, F_RDLCK, SEEK_SET, 0, 100, NULL) ||
        observer_call(&observer, F_SETLK, F_RDLCK, SEEK_SET, 0, 100, &answer) || answer.result)
        return die("read_locks_shared");
    if (record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 100, &own) || own.l_type != F_RDLCK || own.l_pid != observer.pid)
        return die("getlk_other_reader_pid");
    errno = 0;
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 100, NULL) != -1 || !conflict_error(errno))
        return die("write_conflicts_with_reader");
    if (observer_call(&observer, F_SETLK, F_UNLCK, SEEK_SET, 0, 0, &answer) || answer.result || unlock_all(fd))
        return die("clear_read_locks");
    puts("PASS glibc_record_lock_process_ownership");

    if (lseek(fd, 50, SEEK_SET) != 50 || record(fd, F_SETLK, F_WRLCK, SEEK_CUR, 5, -10, NULL) ||
        lseek(fd, 0, SEEK_CUR) != 50 || observer_get(&observer, 40, 20, F_WRLCK, getpid(), 45, 10))
        return die("negative_length_seek_cur");
    if (unlock_all(fd) || record(fd, F_SETLK, F_WRLCK, SEEK_END, -20, 10, NULL) ||
        observer_get(&observer, 490, 22, F_WRLCK, getpid(), 492, 10)) return die("seek_end_normalization");
    if (unlock_all(fd) || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 200, 0, NULL) ||
        observer_get(&observer, 10000, 1, F_WRLCK, getpid(), 200, 0)) return die("length_zero_to_eof");
    if (unlock_all(fd)) return die("clear_eof");
    errno = 0;
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, -1, NULL) != -1 || errno != EINVAL)
        return die("negative_resulting_start");
    errno = 0;
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, INT64_MAX, 2, NULL) != -1 || errno != EOVERFLOW)
        return die("lock_range_overflow");
    puts("PASS glibc_record_lock_range_normalization");

    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 100, NULL) ||
        record(fd, F_SETLK, F_UNLCK, SEEK_SET, 30, 40, NULL) ||
        observer_get(&observer, 30, 40, F_UNLCK, 0, 0, 0) ||
        observer_get(&observer, 0, 1, F_WRLCK, getpid(), 0, 30) ||
        observer_get(&observer, 70, 1, F_WRLCK, getpid(), 70, 30)) return die("unlock_splits_range");
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 30, 40, NULL) ||
        observer_get(&observer, 50, 1, F_WRLCK, getpid(), 0, 100)) return die("adjacent_range_merge");
    if (record(fd, F_SETLK, F_RDLCK, SEEK_SET, 20, 60, NULL) ||
        observer_get(&observer, 30, 1, F_RDLCK, getpid(), 20, 60) ||
        observer_get(&observer, 0, 1, F_WRLCK, getpid(), 0, 20) ||
        observer_get(&observer, 90, 1, F_WRLCK, getpid(), 80, 20) || unlock_all(fd))
        return die("type_conversion_splits_ranges");
    puts("PASS glibc_record_lock_split_merge");

    int unrelated = open(filename, O_RDONLY);
    if (unrelated < 0 || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || close(unrelated) ||
        observer_get(&observer, 0, 1, F_UNLCK, 0, 0, 0)) return die("any_same_inode_close_releases");
    int duplicate = dup(fd);
    if (duplicate < 0 || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || close(duplicate) ||
        observer_get(&observer, 0, 1, F_UNLCK, 0, 0, 0)) return die("duplicate_close_releases");
    if (link(filename, "record-lock-alias")) return die("hardlink_alias_create");
    int alias = open("record-lock-alias", O_RDONLY);
    if (alias < 0 || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || close(alias) ||
        observer_get(&observer, 0, 1, F_UNLCK, 0, 0, 0) || unlink("record-lock-alias"))
        return die("hardlink_alias_close_releases");
    int target = open(filename, O_RDONLY), other = open("record-lock-other", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (target < 0 || other < 0 || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) ||
        dup2(other, target) != target || observer_get(&observer, 0, 1, F_UNLCK, 0, 0, 0) ||
        close(target) || close(other) || unlink("record-lock-other")) return die("dup2_target_close_releases");
    puts("PASS glibc_record_lock_close_dup2_release");
    void *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return die("lock_buffer_map");
    struct flock input; memset(&input, 0, sizeof(input));
    input.l_type = F_WRLCK; input.l_whence = SEEK_SET; input.l_start = 300; input.l_len = 10;
    memcpy(page, &input, sizeof(input));
    if (mprotect(page, 4096, PROT_READ) || fcntl(fd, F_SETLK, page) ||
        observer_get(&observer, 300, 1, F_WRLCK, getpid(), 300, 10)) return die("readonly_setlk_input");
    errno = 0;
    if (fcntl(fd, F_GETLK, page) != -1 || errno != EFAULT || memcmp(page, &input, sizeof(input)) || unlock_all(fd) ||
        mprotect(page, 4096, PROT_READ | PROT_WRITE) || mprotect((char *)page + 4096, 4096, PROT_NONE))
        return die("getlk_requires_writable_output");
    struct flock *boundary = (void *)((char *)page + 4096 - sizeof(input));
    input.l_type = F_WRLCK; input.l_whence = SEEK_CUR; input.l_start = 17; input.l_len = 2; input.l_pid = 1234;
    memcpy(boundary, &input, sizeof(input));
    if (fcntl(fd, F_GETLK, boundary) || boundary->l_type != F_UNLCK ||
        boundary->l_whence != input.l_whence || boundary->l_start != input.l_start ||
        boundary->l_len != input.l_len || boundary->l_pid != input.l_pid) return die("guard_boundary_getlk_preserves_input");
    errno = 0;
    if (fcntl(fd, F_GETLK, (char *)boundary + 1) != -1 || errno != EFAULT || munmap(page, 8192))
        return die("cross_guard_flock_efault");
    int read_only = open(filename, O_RDONLY), write_only = open(filename, O_WRONLY);
    if (read_only < 0 || write_only < 0) return die("mode_descriptors");
    errno = 0;
    if (record(read_only, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) != -1 || errno != EBADF)
        return die("write_lock_requires_writable_fd");
    errno = 0;
    if (record(write_only, F_SETLK, F_RDLCK, SEEK_SET, 0, 1, NULL) != -1 || errno != EBADF ||
        record(write_only, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &own) || own.l_type != F_UNLCK ||
        close(read_only) || close(write_only)) return die("read_lock_requires_readable_fd");
    puts("PASS glibc_record_lock_buffer_access");
#ifdef __arm__
    /* ARM SYS_fcntl uses 16-byte off_t32 flock; fcntl64 commands 12/13/14
     * use the 32-byte aligned off_t64 UAPI. Do not reuse libc's remapped
     * F_GETLK macro for the deliberately old syscall below. */
    struct arm_flock32 { int16_t type, whence; int32_t start, length, pid; };
    _Static_assert(sizeof(struct arm_flock32) == 16 && offsetof(struct arm_flock32, start) == 4,
                   "ARM old Linux flock layout");
    _Static_assert(sizeof(struct flock) == 32 && offsetof(struct flock, l_start) == 8,
                   "ARM large-file Linux flock64 layout");
    struct arm_flock32 old = { F_WRLCK, SEEK_SET, 300, 10, 0 };
    if (syscall(SYS_fcntl, fd, 6, &old) || syscall(SYS_fcntl, fd, 7, &old) ||
        observer_get(&observer, 300, 1, F_WRLCK, getpid(), 300, 10) || unlock_all(fd))
        return die("arm_old_setlk_layout");
    if (observer_call(&observer, F_SETLK, F_WRLCK, SEEK_SET, 300, 10, &answer) || answer.result)
        return die("arm_old_getlk_setup");
    old = (struct arm_flock32){ F_WRLCK, SEEK_SET, 0, 0, 0 };
    if (syscall(SYS_fcntl, fd, 5, &old) || old.type != F_WRLCK || old.whence != SEEK_SET ||
        old.start != 300 || old.length != 10 || old.pid != observer.pid) return die("arm_old_getlk_layout");
    if (observer_call(&observer, F_SETLK, F_UNLCK, SEEK_SET, 0, 0, &answer) || answer.result)
        return die("arm_old_clear");
    struct flock wide; memset(&wide, 0, sizeof(wide));
    wide.l_type = F_WRLCK; wide.l_whence = SEEK_SET; wide.l_start = INT64_C(0x100000008); wide.l_len = 16;
    if (syscall(SYS_fcntl64, fd, 13, &wide) ||
        observer_get(&observer, wide.l_start, 1, F_WRLCK, getpid(), wide.l_start, 16) || unlock_all(fd))
        return die("arm_wide_setlk_above_4g");
    if (syscall(SYS_fcntl64, fd, 14, &wide) || unlock_all(fd)) return die("arm_wide_setlkw_layout");
    if (observer_call(&observer, F_SETLK, F_WRLCK, SEEK_SET, INT64_C(0x100000008), 16, &answer) || answer.result)
        return die("arm_old_getlk_overflow_setup");
    memset(&wide, 0, sizeof(wide)); wide.l_type = F_WRLCK; wide.l_whence = SEEK_SET;
    if (syscall(SYS_fcntl64, fd, 12, &wide) || wide.l_start != INT64_C(0x100000008) ||
        wide.l_len != 16 || wide.l_pid != observer.pid) return die("arm_wide_getlk_above_4g");
    old = (struct arm_flock32){ F_WRLCK, SEEK_SET, 0, 0, 1234 };
    struct arm_flock32 unchanged = old;
    errno = 0;
    if (syscall(SYS_fcntl, fd, 5, &old) != -1 || errno != EOVERFLOW || memcmp(&old, &unchanged, sizeof(old)))
        return die("arm_old_getlk_eoverflow_no_partial_output");
    if (observer_call(&observer, F_SETLK, F_UNLCK, SEEK_SET, 0, 0, &answer) || answer.result)
        return die("arm_wide_clear");
    puts("PASS glibc_record_lock_arm_old_wide_layout");
#endif
    if (observer_stop(&observer)) return die("observer_exit");
    return 0;
}

static void signal_handler(int number) { (void)number; received++; }
struct blocked { pid_t pid; int commands, replies; };
static struct blocked blocker_start_flags(int fd, int restart)
{
    int commands[2], replies[2]; struct blocked process = { -1, -1, -1 };
    if (pipe(commands) || pipe(replies)) return process;
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask); action.sa_flags = restart ? SA_RESTART : 0; received = 0;
        if (sigaction(SIGUSR1, &action, NULL) || byte_out(replies[1], 'r')) _exit(10);
        errno = 0;
        int rc = record(fd, F_SETLKW, F_WRLCK, SEEK_SET, 0, 1, NULL);
        struct response result = { .result = rc, .error = errno, .pid = received };
        if (transfer(replies[1], &result, sizeof(result), 1) || byte_in(commands[0], 'x')) _exit(11);
        _exit(0);
    }
    close(commands[0]); close(replies[1]);
    process.pid = child; process.commands = commands[1]; process.replies = replies[0]; return process;
}
static struct blocked blocker_start(int fd) { return blocker_start_flags(fd, 0); }
static int assert_blocked(struct blocked *process)
{
    if (process->pid < 0 || byte_in(process->replies, 'r')) return -1;
    struct pollfd descriptor = {process->replies, POLLIN, 0};
    return poll(&descriptor, 1, 50) == 0 ? 0 : -1;
}
static int blocker_stop(struct blocked *process)
{
    int rc = byte_out(process->commands, 'x'); close(process->commands); close(process->replies);
    return rc || wait_ok(process->pid) ? -1 : 0;
}
static int blocked_lifecycle(int fd)
{
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("wait_owner");
    struct blocked process = blocker_start(fd); struct response answer;
    if (assert_blocked(&process) || unlock_all(fd) || transfer(process.replies, &answer, sizeof(answer), 0) || answer.result)
        return die("setlkw_unlock_wakeup");
    struct flock held;
    if (record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &held) || held.l_type != F_WRLCK || held.l_pid != process.pid ||
        blocker_stop(&process) || record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &held) || held.l_type != F_UNLCK)
        return die("wait_commit_owner_exit_release");
    puts("PASS glibc_record_lock_wait_wake_owner_exit");

    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("signal_owner");
    process = blocker_start(fd);
    if (assert_blocked(&process) || kill(process.pid, SIGUSR1) || transfer(process.replies, &answer, sizeof(answer), 0) ||
        answer.result != -1 || answer.error != EINTR || answer.pid != 1 || unlock_all(fd) ||
        record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &held) || held.l_type != F_UNLCK ||
        record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || blocker_stop(&process) || unlock_all(fd))
        return die("interrupted_wait_not_granted_later");
    puts("PASS glibc_record_lock_wait_signal_eintr");

    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("restart_owner");
    process = blocker_start_flags(fd, 1);
    if (assert_blocked(&process) || kill(process.pid, SIGUSR1)) return die("restart_signal");
    struct pollfd restarted = {process.replies, POLLIN, 0};
    if (poll(&restarted, 1, 50) || unlock_all(fd) || transfer(process.replies, &answer, sizeof(answer), 0) ||
        answer.result || answer.pid != 1 || blocker_stop(&process)) return die("setlkw_sa_restart");
    puts("PASS glibc_record_lock_wait_signal_restart");

    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("killed_wait_owner");
    process = blocker_start(fd);
    int status;
    if (assert_blocked(&process) || kill(process.pid, SIGKILL) || waitpid(process.pid, &status, 0) != process.pid ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL || unlock_all(fd)) return die("killed_waiter_exit");
    close(process.commands); close(process.replies);
    if (record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &held) || held.l_type != F_UNLCK ||
        record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || unlock_all(fd)) return die("killed_waiter_reclaimed");
    puts("PASS glibc_record_lock_waiter_exit_reclaims");
    return 0;
}

static int deadlock_detection(int fd)
{
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("deadlock_first_owner");
    int commands[2], replies[2]; if (pipe(commands) || pipe(replies)) return die("deadlock_pipes");
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 1, 1, NULL) || byte_out(replies[1], 'r')) _exit(40);
        int rc = record(fd, F_SETLKW, F_WRLCK, SEEK_SET, 0, 1, NULL);
        struct response response = { .result = rc, .error = errno };
        if (transfer(replies[1], &response, sizeof(response), 1) || byte_in(commands[0], 'x')) _exit(41);
        _exit(0);
    }
    close(commands[0]); close(replies[1]);
    struct blocked blocked = { child, commands[1], replies[0] };
    if (assert_blocked(&blocked)) return die("deadlock_peer_waiting");
    struct sigaction action, saved; memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler; sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, &saved)) return die("deadlock_watchdog");
    alarm(2); errno = 0;
    int result = record(fd, F_SETLKW, F_WRLCK, SEEK_SET, 1, 1, NULL), error = errno;
    alarm(0); sigaction(SIGALRM, &saved, NULL);
    if (unlock_all(fd)) return die("deadlock_release_first_owner");
    struct response response;
    if (transfer(replies[0], &response, sizeof(response), 0) || response.result || blocker_stop(&blocked))
        return die("deadlock_peer_recovered");
    if (result != -1 || error != EDEADLK) { errno = error; return die("cyclic_wait_returns_edeadlk"); }
    puts("PASS glibc_record_lock_deadlock_detection");
    return 0;
}

static int cancellation_fd, cancellation_ready, cancellation_cleanup;
static void cancelled_cleanup(void *unused)
{ (void)unused; __atomic_add_fetch(&cancellation_cleanup, 1, __ATOMIC_RELEASE); }
static void *cancel_waiter(void *unused)
{
    (void)unused;
    pthread_cleanup_push(cancelled_cleanup, NULL);
    __atomic_store_n(&cancellation_ready, 1, __ATOMIC_RELEASE);
    int rc = record(cancellation_fd, F_SETLKW, F_WRLCK, SEEK_SET, 0, 1, NULL);
    if (rc) return (void *)2;
    pthread_cleanup_pop(0);
    return NULL;
}
static int cancelled_wait(int fd)
{
    if (record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL)) return die("cancel_owner");
    int commands[2], replies[2]; if (pipe(commands) || pipe(replies)) return die("cancel_pipes");
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]); cancellation_fd = fd;
        pthread_t waiter;
        if (pthread_create(&waiter, NULL, cancel_waiter, NULL)) _exit(20);
        while (!__atomic_load_n(&cancellation_ready, __ATOMIC_ACQUIRE)) {
            struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
        }
        if (byte_out(replies[1], 'r') || byte_in(commands[0], 'c')) _exit(21);
        void *result;
        if (pthread_cancel(waiter) || pthread_join(waiter, &result) || result != PTHREAD_CANCELED || cancellation_cleanup != 1 ||
            byte_out(replies[1], 'd') || byte_in(commands[0], 'x')) _exit(22);
        _exit(0);
    }
    close(commands[0]); close(replies[1]);
    if (child < 0 || byte_in(replies[0], 'r')) return die("cancel_worker_ready");
    struct pollfd pending = {replies[0], POLLIN, 0};
    if (poll(&pending, 1, 50) || byte_out(commands[1], 'c') || byte_in(replies[0], 'd') || unlock_all(fd))
        return die("cancel_blocked_fcntl_cleanup");
    struct flock query;
    if (record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &query) || query.l_type != F_UNLCK ||
        record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || byte_out(commands[1], 'x') ||
        wait_ok(child) || unlock_all(fd)) return die("cancelled_wait_not_granted_later");
    close(commands[1]); close(replies[0]);
    puts("PASS glibc_record_lock_wait_cancel_reclaims");
    return 0;
}

static int exec_child(int argc, char **argv)
{
    if (argc != 6) return die("exec_arguments");
    int retained = atoi(argv[2]), closed = atoi(argv[3]), response = atoi(argv[4]), command = atoi(argv[5]);
    errno = 0;
    if (fcntl(retained, F_GETFD) < 0 || fcntl(closed, F_GETFD) != -1 || errno != EBADF ||
        byte_out(response, 'e') || byte_in(command, 'x')) return die("exec_descriptor_state");
    return 0;
}
static int exec_release(int fd, const char *program)
{
    int commands[2], replies[2]; if (pipe(commands) || pipe(replies)) return die("exec_pipes");
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        int cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (cloexec < 0 || record(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1, NULL) || byte_out(replies[1], 'l') ||
            byte_in(commands[0], 'g')) _exit(30);
        char keep[16], drop[16], out[16], in[16];
        snprintf(keep, sizeof(keep), "%d", fd); snprintf(drop, sizeof(drop), "%d", cloexec);
        snprintf(out, sizeof(out), "%d", replies[1]); snprintf(in, sizeof(in), "%d", commands[0]);
        execl(program, program, "exec-lock-check", keep, drop, out, in, (char *)NULL); _exit(31);
    }
    close(commands[0]); close(replies[1]);
    struct flock query;
    if (child < 0 || byte_in(replies[0], 'l') || record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &query) ||
        query.l_type != F_WRLCK || query.l_pid != child || byte_out(commands[1], 'g') || byte_in(replies[0], 'e') ||
        record(fd, F_GETLK, F_WRLCK, SEEK_SET, 0, 1, &query) || query.l_type != F_UNLCK ||
        byte_out(commands[1], 'x') || wait_ok(child)) return die("exec_cloexec_any_close_releases");
    close(commands[1]); close(replies[0]);
    puts("PASS glibc_record_lock_exec_cloexec_release");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exec-lock-check")) return exec_child(argc, argv);
    int fd = open(filename, O_CREAT | O_EXCL | O_RDWR, 0600);
    char bytes[512]; memset(bytes, '.', sizeof(bytes));
    if (fd < 0 || write(fd, bytes, sizeof(bytes)) != sizeof(bytes)) return die("file_setup");
    int result = ranges_and_ownership(fd);
    if (!result) result = blocked_lifecycle(fd);
    if (!result) result = deadlock_detection(fd);
    if (!result) result = cancelled_wait(fd);
    if (!result) result = exec_release(fd, argv[0]);
    if (close(fd) || unlink(filename)) return die("file_cleanup");
    return result;
}
