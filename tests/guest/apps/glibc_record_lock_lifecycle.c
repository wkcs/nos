/* Copyright (C) 2026 NOS contributors. POSIX record-lock lifetime checks. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char file_a[] = "record-lifecycle-a";
static const char file_b[] = "record-lifecycle-b";
static const char alias_a[] = "record-lifecycle-alias";
static const char moved_a[] = "record-lifecycle-moved";
static int die(const char *step)
{ fprintf(stderr, "FAIL glibc_record_lock_lifecycle %s errno=%d\n", step, errno); return 96; }
static int byte_out(int fd, char byte)
{
    ssize_t n;
    do n = write(fd, &byte, 1); while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static int byte_in(int fd, char expected)
{
    struct pollfd input = {fd, POLLIN, 0};
    int rc;
    do rc = poll(&input, 1, 10000); while (rc < 0 && errno == EINTR);
    if (!rc) { errno = ETIMEDOUT; return -1; }
    if (rc != 1) return -1;
    char byte; ssize_t n;
    do n = read(fd, &byte, 1); while (n < 0 && errno == EINTR);
    if (n != 1) { if (!n) errno = EPIPE; return -1; }
    if (byte != expected) { errno = EPROTO; return -1; }
    return 0;
}
static int wait_ok(pid_t child)
{
    int status; pid_t rc;
    do rc = waitpid(child, &status, 0); while (rc < 0 && errno == EINTR);
    return rc == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
static int lock_range(int fd, off_t start, off_t length)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = start, .l_len = length};
    return fcntl(fd, F_SETLK, &lock);
}
/* Call in a process different from the owner, since GETLK ignores own locks. */
static int seen(int fd, short type, pid_t owner, off_t start, off_t length)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = start, .l_len = length};
    if (fcntl(fd, F_GETLK, &lock) || lock.l_type != type) return -1;
    return type == F_UNLCK || lock.l_pid == owner ? 0 : -1;
}
static int probe(int fd, short type, pid_t owner, off_t start, off_t length)
{
    pid_t child = fork();
    if (!child) _exit(seen(fd, type, owner, start, length) ? 2 : 0);
    return child < 0 ? -1 : wait_ok(child);
}
static int new_file(const char *path)
{ return open(path, O_CREAT | O_EXCL | O_RDWR, 0600); }

struct exec_fds { int a, b, extra, closing, commands, replies; pid_t pid; };
static int replace(const char *program, const char *stage, const struct exec_fds *f)
{
    char a[16], b[16], extra[16], closing[16], commands[16], replies[16], pid[16];
    snprintf(a, sizeof(a), "%d", f->a); snprintf(b, sizeof(b), "%d", f->b);
    snprintf(extra, sizeof(extra), "%d", f->extra); snprintf(closing, sizeof(closing), "%d", f->closing);
    snprintf(commands, sizeof(commands), "%d", f->commands); snprintf(replies, sizeof(replies), "%d", f->replies);
    snprintf(pid, sizeof(pid), "%ld", (long)f->pid);
    execl(program, program, stage, a, b, extra, closing, commands, replies, pid, (char *)NULL);
    return die("exec_replace");
}
static int exec_stage(int argc, char **argv)
{
    if (argc != 9) return die("exec_stage_arguments");
    struct exec_fds f = {atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
        atoi(argv[6]), atoi(argv[7]), (pid_t)strtol(argv[8], NULL, 10)};
    if (getpid() != f.pid || fcntl(f.a, F_GETFD) < 0 || fcntl(f.extra, F_GETFD) < 0 ||
        fcntl(f.b, F_GETFD) < 0) return die("exec_pid_and_retained_descriptors");
    if (!strcmp(argv[1], "exec-preserve")) {
        if (fcntl(f.closing, F_GETFD) != 0 || byte_out(f.replies, '1') || byte_in(f.commands, 'g') ||
            fcntl(f.closing, F_SETFD, FD_CLOEXEC)) return die("exec_preserve_handshake");
        return replace(argv[0], "exec-close", &f);
    }
    errno = 0;
    if (fcntl(f.closing, F_GETFD) != -1 || errno != EBADF || byte_out(f.replies, '2') ||
        byte_in(f.commands, 'x')) return die("exec_cloexec_descriptor_closed");
    return 0;
}
static int exec_lifecycle(const char *program)
{
    int a = new_file(file_a), b = new_file(file_b), extra = open(file_a, O_RDWR), closing = open(file_a, O_RDWR);
    int commands[2], replies[2];
    if (a < 0 || b < 0 || extra < 0 || closing < 0 || pipe(commands) || pipe(replies)) return die("exec_setup");
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        struct exec_fds f = {a, b, extra, closing, commands[0], replies[1], getpid()};
        if (lock_range(a, 0, 4) || lock_range(extra, 100, 4) || lock_range(b, 50, 4) ||
            byte_out(f.replies, 'l') || byte_in(f.commands, 'g') || fcntl(closing, F_SETFD, FD_CLOEXEC))
            _exit(die("child_lock_setup"));
        char *missing[] = {"record-lifecycle-no-such-executable", NULL};
        errno = 0;
        if (execv(missing[0], missing) != -1 || errno != ENOENT || getpid() != f.pid ||
            fcntl(closing, F_GETFD) != FD_CLOEXEC) _exit(die("failed_exec_missing_preserves_fd"));
        volatile uintptr_t invalid_vector = 1;
        extern char **environ;
        errno = 0;
        if (execve(program, (char *const *)invalid_vector, environ) != -1 || errno != EFAULT || getpid() != f.pid ||
            fcntl(closing, F_GETFD) != FD_CLOEXEC || byte_out(f.replies, 'f') || byte_in(f.commands, 'g') ||
            fcntl(closing, F_SETFD, 0)) _exit(die("failed_exec_bad_vector_preserves_fd"));
        _exit(replace(program, "exec-preserve", &f));
    }
    close(commands[0]); close(replies[1]);
    if (child < 0) return die("exec_fork");
    if (byte_in(replies[0], 'l')) return die("exec_initial_ready");
    if (close(extra)) return die("exec_parent_close_extra");
    if (close(closing)) return die("exec_parent_close_closing");
    if (seen(a, F_WRLCK, child, 0, 4)) return die("exec_initial_a_first_lock");
    if (seen(a, F_WRLCK, child, 100, 4)) return die("exec_initial_a_second_lock");
    if (seen(b, F_WRLCK, child, 50, 4)) return die("exec_initial_b_lock");
    if (byte_out(commands[1], 'g')) return die("exec_allow_failure_probes");
    if (byte_in(replies[0], 'f')) return die("exec_failed_probes_ready");
    if (seen(a, F_WRLCK, child, 0, 4)) return die("exec_failed_a_first_lock");
    if (seen(a, F_WRLCK, child, 100, 4)) return die("exec_failed_a_second_lock");
    if (seen(b, F_WRLCK, child, 50, 4)) return die("exec_failed_b_lock");
    if (byte_out(commands[1], 'g')) return die("exec_allow_preserving_replace");
    if (byte_in(replies[0], '1')) return die("exec_preserving_replace_ready");
    if (seen(a, F_WRLCK, child, 0, 4)) return die("exec_preserved_a_first_lock");
    if (seen(a, F_WRLCK, child, 100, 4)) return die("exec_preserved_a_second_lock");
    if (seen(b, F_WRLCK, child, 50, 4)) return die("exec_preserved_b_lock");
    puts("PASS glibc_record_exec_preserves_and_failed_exec");
    if (byte_out(commands[1], 'g')) return die("exec_allow_cloexec_replace");
    if (byte_in(replies[0], '2')) return die("exec_cloexec_replace_ready");
    if (seen(a, F_UNLCK, 0, 0, 4)) return die("exec_cloexec_a_first_released");
    if (seen(a, F_UNLCK, 0, 100, 4)) return die("exec_cloexec_a_second_released");
    if (seen(b, F_WRLCK, child, 50, 4)) return die("exec_cloexec_b_preserved");
    if (byte_out(commands[1], 'x')) return die("exec_allow_child_exit");
    if (wait_ok(child)) return die("exec_child_exit_status");
    if (seen(b, F_UNLCK, 0, 50, 4)) return die("exec_child_exit_b_released");
    close(commands[1]); close(replies[0]);
    if (close(a) || close(b) || unlink(file_a) || unlink(file_b)) return die("exec_cleanup");
    puts("PASS glibc_record_exec_cloexec_inode_scope");
    return 0;
}

static int alias_lifecycle(void)
{
    pid_t owner = getpid();
    int a = new_file(file_a);
    if (a < 0 || link(file_a, alias_a)) return die("alias_setup");
    int alias = open(alias_a, O_RDWR);
    if (alias < 0 || lock_range(a, 0, 16) || probe(alias, F_WRLCK, owner, 0, 16) ||
        rename(file_a, moved_a) || probe(alias, F_WRLCK, owner, 0, 16) || unlink(alias_a) || unlink(moved_a))
        return die("alias_rename_unlink_keeps_lock");
    struct stat old, fresh;
    int replacement = new_file(file_a);
    if (replacement < 0 || fstat(a, &old) || fstat(replacement, &fresh) || old.st_nlink || old.st_ino == fresh.st_ino ||
        probe(a, F_WRLCK, owner, 0, 16) || probe(replacement, F_UNLCK, 0, 0, 16) || close(replacement) ||
        probe(a, F_WRLCK, owner, 0, 16) || unlink(file_a) || close(alias) || probe(a, F_UNLCK, 0, 0, 16) || close(a))
        return die("unlinked_inode_and_same_name_replacement");
    puts("PASS glibc_record_alias_unlink_recreate");
    /* Repeatedly free the vnode before recreating it; VFS first-free allocation
     * must not leave lock state attached to its reused node slot. */
    for (unsigned i = 0; i < 8; i++) {
        a = new_file(file_a);
        if (a < 0 || probe(a, F_UNLCK, 0, 0, 16) || lock_range(a, 0, 16) || close(a)) return die("reuse_lock_setup");
        a = open(file_a, O_RDWR);
        if (a < 0 || probe(a, F_UNLCK, 0, 0, 16) || close(a) || unlink(file_a)) return die("reuse_no_ghost_lock");
    }
    puts("PASS glibc_record_inode_reuse_cleanup");
    return 0;
}
static int rename_overwrite(void)
{
    int source = new_file(file_a), victim = new_file(file_b), commands[2], replies[2];
    if (source < 0 || victim < 0 || pipe(commands) || pipe(replies)) return die("rename_setup");
    pid_t child = fork();
    if (!child) {
        close(commands[1]); close(replies[0]);
        if (lock_range(victim, 32, 8) || byte_out(replies[1], 'l') || byte_in(commands[0], 'x'))
            _exit(die("rename_victim_owner"));
        _exit(0);
    }
    close(commands[0]); close(replies[1]);
    if (child < 0 || byte_in(replies[0], 'l') || lock_range(source, 0, 8) || rename(file_a, file_b))
        return die("rename_overwrite_locked_victim");
    int new_path = open(file_b, O_RDWR), old_alias = dup(victim);
    struct stat replaced, current;
    if (new_path < 0 || old_alias < 0 || fstat(victim, &replaced) || fstat(new_path, &current) ||
        replaced.st_ino == current.st_ino || probe(new_path, F_WRLCK, getpid(), 0, 8) ||
        seen(victim, F_WRLCK, child, 32, 8) || close(new_path) || probe(source, F_UNLCK, 0, 0, 8) ||
        seen(victim, F_WRLCK, child, 32, 8) || lock_range(source, 0, 8) || close(victim) ||
        seen(old_alias, F_WRLCK, child, 32, 8) || probe(source, F_WRLCK, getpid(), 0, 8))
        return die("rename_two_inode_lock_identity");
    if (byte_out(commands[1], 'x') || wait_ok(child) || seen(old_alias, F_UNLCK, 0, 32, 8) ||
        close(old_alias) || close(source) || unlink(file_b)) return die("rename_owner_exit_cleanup");
    close(commands[1]); close(replies[0]);
    puts("PASS glibc_record_rename_overwrite_inode_scope");
    return 0;
}
static int dup_lifecycle(void)
{
    int a = new_file(file_a), b = new_file(file_b);
    if (a < 0 || b < 0) return die("dup_setup");
    int target = dup(a);
    if (target < 0 || lock_range(a, 0, 4) || lock_range(a, 100, 4) || lock_range(b, 50, 4) ||
        fcntl(a, F_SETFD, FD_CLOEXEC) || fcntl(target, F_SETFD, FD_CLOEXEC) || dup2(a, a) != a ||
        fcntl(a, F_GETFD) != FD_CLOEXEC || probe(a, F_WRLCK, getpid(), 0, 4) ||
        probe(a, F_WRLCK, getpid(), 100, 4)) return die("dup2_same_fd_no_close");
    errno = 0;
    if (dup2(-1, target) != -1 || errno != EBADF || fcntl(target, F_GETFD) != FD_CLOEXEC ||
        probe(a, F_WRLCK, getpid(), 0, 4) || probe(a, F_WRLCK, getpid(), 100, 4))
        return die("dup2_bad_source_preserves_target");
    puts("PASS glibc_record_dup2_identity_and_failed_source");
    if (dup2(a, target) != target || fcntl(target, F_GETFD) != 0 || fcntl(a, F_GETFD) != FD_CLOEXEC ||
        probe(a, F_UNLCK, 0, 0, 4) || probe(a, F_UNLCK, 0, 100, 4) ||
        probe(b, F_WRLCK, getpid(), 50, 4)) return die("dup2_same_ofd_target_actually_closes");
    if (close(target) || close(a) || close(b) || unlink(file_a) || unlink(file_b)) return die("dup_cleanup");
    puts("PASS glibc_record_dup2_same_ofd_target_close");
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "exec-preserve") || !strcmp(argv[1], "exec-close"))) return exec_stage(argc, argv);
    int rc = exec_lifecycle(argv[0]);
    if (!rc) rc = alias_lifecycle();
    if (!rc) rc = rename_overwrite();
    if (!rc) rc = dup_lifecycle();
    return rc;
}
