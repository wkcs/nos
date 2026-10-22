/* Copyright (C) 2026 NOS contributors. Real glibc DAC and signal permissions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE "/glibc-permissions"
#define FILE BASE "/file"
#define ALIAS BASE "/alias"
#define STICKY BASE "/sticky"
#define CHECK(condition, step) do { if (!(condition)) return fail(step); } while (0)
#define ERROR(operation, code, step) do { errno = 0; CHECK((operation) == -1 && errno == (code), step); } while (0)

static int inherited_fd = -1;
static pid_t signal_target;
static int fail(const char *step)
{
    fprintf(stderr, "FAIL glibc_permissions %s errno=%d\n", step, errno);
    return 98;
}
static int put_byte(int fd, char byte)
{
    ssize_t n;
    do n = write(fd, &byte, 1); while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static int get_byte(int fd, char expected)
{
    struct pollfd p = {fd, POLLIN, 0};
    int rc;
    do rc = poll(&p, 1, 10000); while (rc < 0 && errno == EINTR);
    if (rc == 0) { errno = ETIMEDOUT; return -1; }
    if (rc != 1) return -1;
    char byte;
    ssize_t n;
    do n = read(fd, &byte, 1); while (n < 0 && errno == EINTR);
    if (n != 1) { if (!n) errno = EPIPE; return -1; }
    if (byte != expected) { errno = EPROTO; return -1; }
    return 0;
}
static int reap(pid_t child)
{
    int status;
    pid_t rc;
    do rc = waitpid(child, &status, 0); while (rc < 0 && errno == EINTR);
    if (rc != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        errno = ECHILD;
        return -1;
    }
    return 0;
}
static int drop(uid_t uid, gid_t gid, gid_t supplementary)
{
    size_t count = supplementary == (gid_t)-1 ? 0 : 1;
    return setgroups(count, count ? &supplementary : NULL) ||
        setresgid(gid, gid, gid) || setresuid(uid, uid, uid) ? -1 : 0;
}
/* Every fork has a ten-second pipe completion bound; no timing guesses. */
static int run_child(int (*test)(void), uid_t uid, gid_t gid, gid_t supplementary)
{
    int ready[2];
    CHECK(!pipe(ready), "child_pipe");
    pid_t child = fork();
    if (!child) {
        close(ready[0]);
        int rc = uid == (uid_t)-1 || !drop(uid, gid, supplementary) ? test() : fail("drop_ids");
        if (!rc && put_byte(ready[1], 'p')) rc = fail("child_complete");
        _exit(rc);
    }
    close(ready[1]);
    if (child < 0) { close(ready[0]); return fail("child_fork"); }
    int rc = get_byte(ready[0], 'p');
    close(ready[0]);
    if (rc) kill(child, SIGKILL);
    int status = reap(child);
    CHECK(!rc && !status, "child_result");
    return 0;
}
static int create_file(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);
    CHECK(fd >= 0, "create_file");
    CHECK(write(fd, "data", 4) == 4 && !fchown(fd, uid, gid) && !fchmod(fd, mode) && !close(fd),
          "create_file_metadata");
    return 0;
}
static int can_read(void)
{
    int fd = open(FILE, O_RDONLY);
    CHECK(fd >= 0 && !close(fd), "read_allowed");
    return 0;
}
static int can_read_write(void)
{
    int fd = open(FILE, O_RDWR);
    CHECK(fd >= 0 && !close(fd), "owner_read_write");
    return 0;
}
static int read_only(void)
{
    CHECK(!can_read(), "group_read");
    ERROR(open(FILE, O_WRONLY), EACCES, "group_write_denied");
    ERROR(open(FILE, O_WRONLY | O_TRUNC), EACCES, "group_truncate_denied");
    struct stat s;
    CHECK(!stat(FILE, &s) && s.st_size == 4, "denied_truncate_keeps_size");
    return 0;
}
static int cannot_read(void)
{
    ERROR(open(FILE, O_RDONLY), EACCES, "class_read_denied");
    return 0;
}
static int umask_create(void)
{
    mode_t previous = umask(0027);
    int fd = open(BASE "/masked", O_CREAT | O_EXCL | O_RDWR, 0666);
    struct stat s;
    CHECK(fd >= 0 && !fstat(fd, &s) && s.st_uid == 1001 && s.st_gid == 3000 &&
          (s.st_mode & 07777) == 0640 && !close(fd), "umask_file_owner_mode");
    CHECK(!mkdir(BASE "/masked-dir", 0777) && !stat(BASE "/masked-dir", &s) &&
          (s.st_mode & 07777) == 0750 && !rmdir(BASE "/masked-dir") && !unlink(BASE "/masked"),
          "umask_directory_mode");
    umask(previous);
    return 0;
}
static int dac_modes(void)
{
    CHECK(!create_file(FILE, 1001, 2001, 0640), "dac_setup");
    CHECK(!run_child(can_read_write, 1001, 3000, (gid_t)-1) &&
          !run_child(read_only, 1002, 2001, (gid_t)-1) &&
          !run_child(read_only, 1002, 3000, 2001) &&
          !run_child(cannot_read, 1004, 3000, (gid_t)-1), "dac_classes");
    CHECK(!chmod(FILE, 0004) && !run_child(can_read, 1004, 3000, (gid_t)-1) &&
          !run_child(cannot_read, 1001, 3000, (gid_t)-1) &&
          !run_child(cannot_read, 1002, 2001, (gid_t)-1), "dac_class_priority");
    CHECK(!run_child(umask_create, 1001, 3000, (gid_t)-1) && !unlink(FILE), "dac_cleanup");
    puts("PASS glibc_permissions_owner_group_supplementary_other_umask");
    return 0;
}
static int access_split(void)
{
    CHECK(!setgroups(0, NULL) && !setresgid(2001, 3000, 3000) && !setresuid(1001, 1002, 1002),
          "access_split_ids");
    CHECK(!access(FILE, R_OK), "access_real_uid");
    ERROR(faccessat(AT_FDCWD, FILE, R_OK, AT_EACCESS), EACCES, "access_effective_uid_denied");
    CHECK(!faccessat(AT_FDCWD, ALIAS, R_OK, AT_EACCESS), "access_effective_other_allowed");
    ERROR(access(ALIAS, R_OK), EACCES, "access_real_owner_denied");
    return 0;
}
static int access_modes(void)
{
    CHECK(!create_file(FILE, 1001, 2001, 0400) && !create_file(ALIAS, 1001, 2001, 0004),
          "access_setup");
    CHECK(!run_child(access_split, (uid_t)-1, 0, 0), "access_split_child");
    CHECK(!chmod(FILE, 0000) && !access(FILE, R_OK | W_OK), "root_access_read_write");
    ERROR(access(FILE, X_OK), EACCES, "root_access_no_execute_bit");
    CHECK(!chmod(FILE, 0001) && !access(FILE, X_OK) && !unlink(FILE) && !unlink(ALIAS),
          "root_access_execute_bit");
    puts("PASS glibc_permissions_access_real_effective_root_execute");
    return 0;
}
static int retained_fd(void)
{
    char data[4];
    CHECK(pread(inherited_fd, data, sizeof(data), 0) == sizeof(data) && !memcmp(data, "data", 4) &&
          pwrite(inherited_fd, "keep", 4, 0) == 4, "opened_descriptor_retains_rights");
    ERROR(open(FILE, O_RDONLY), EACCES, "new_open_after_drop");
    ERROR(fchmod(inherited_fd, 0644), EPERM, "fd_metadata_still_checks_owner");
    return 0;
}
static int descriptor_rights(void)
{
    CHECK(!create_file(FILE, 0, 0, 0600), "descriptor_setup");
    inherited_fd = open(FILE, O_RDWR);
    CHECK(inherited_fd >= 0 && !run_child(retained_fd, 1004, 3000, (gid_t)-1), "descriptor_child");
    char data[4];
    CHECK(pread(inherited_fd, data, 4, 0) == 4 && !memcmp(data, "keep", 4) &&
          !close(inherited_fd) && !unlink(FILE), "descriptor_cleanup");
    inherited_fd = -1;
    puts("PASS glibc_permissions_open_descriptor_survives_drop");
    return 0;
}
static int search_denied(void)
{
    struct stat s;
    ERROR(stat(BASE "/gate/file", &s), EACCES, "search_component");
    ERROR(stat(BASE "/gate/../public", &s), EACCES, "search_before_parent_escape");
    ERROR(stat(BASE "/gate/./file", &s), EACCES, "search_dot_component");
    ERROR(stat(BASE "/gate/", &s), EACCES, "search_final_directory_slash");
    ERROR(open(BASE "/symbol", O_RDONLY), EACCES, "search_symlink_target");
    ERROR(access(BASE "/symbol", R_OK), EACCES, "access_symlink_target");
    CHECK(!faccessat(AT_FDCWD, BASE "/symbol", F_OK, AT_SYMLINK_NOFOLLOW), "access_symlink_itself");
    return 0;
}
static int search_allowed(void)
{
    struct stat s;
    CHECK(!stat(BASE "/gate/./file", &s) && !stat(BASE "/gate/../public", &s), "owner_search");
    ERROR(stat(BASE "/gate/file/", &s), ENOTDIR, "regular_final_slash");
    return 0;
}
static int directory_write_denied(void)
{
    int fd = open(BASE "/gate/file", O_RDONLY);
    CHECK(fd >= 0 && !close(fd), "search_without_parent_write");
    ERROR(open(BASE "/gate/new", O_CREAT | O_EXCL | O_WRONLY, 0600), EACCES,
          "parent_write_create_denied");
    ERROR(unlink(BASE "/gate/file"), EACCES, "parent_write_unlink_denied");
    ERROR(rename(BASE "/gate/file", BASE "/gate/renamed"), EACCES, "parent_write_rename_denied");
    ERROR(mkdir(BASE "/gate/newdir", 0700), EACCES, "parent_write_mkdir_denied");
    ERROR(link(BASE "/gate/file", BASE "/gate/alias"), EACCES, "parent_write_link_denied");
    ERROR(symlink("file", BASE "/gate/newlink"), EACCES, "parent_write_symlink_denied");
    struct stat s;
    CHECK(!stat(BASE "/gate/file", &s) && s.st_size == 4 && s.st_nlink == 1,
          "denied_parent_mutations_preserve_inode");
    return 0;
}
static int component_search(void)
{
    CHECK(!mkdir(BASE "/gate", 0700) && !chown(BASE "/gate", 1001, 2001) &&
          !create_file(BASE "/gate/file", 1001, 2001, 0644) &&
          !create_file(BASE "/public", 0, 0, 0644) && !symlink("gate/file", BASE "/symbol"),
          "search_setup");
    CHECK(!run_child(search_denied, 1004, 3000, (gid_t)-1) &&
          !run_child(search_allowed, 1001, 3000, (gid_t)-1) &&
          !chmod(BASE "/gate", 0555) && !run_child(directory_write_denied, 1004, 3000, (gid_t)-1),
          "search_children");
    CHECK(!unlink(BASE "/symbol") && !unlink(BASE "/public") &&
          !unlink(BASE "/gate/file") && !rmdir(BASE "/gate"), "search_cleanup");
    puts("PASS glibc_permissions_component_search_dot_parent_symlink_slash");
    return 0;
}
static int sticky_unlink_denied(void)
{
    ERROR(unlink(STICKY "/victim"), EPERM, "sticky_unrelated_unlink");
    ERROR(rename(STICKY "/victim", STICKY "/free"), EPERM, "sticky_unrelated_rename_source");
    return 0;
}
static int sticky_unlink_owner(void)
{
    CHECK(!unlink(STICKY "/victim"), "sticky_file_or_directory_owner_unlink");
    return 0;
}
static int sticky_rename_denied(void)
{
    ERROR(rename(STICKY "/owned", STICKY "/victim"), EPERM, "sticky_rename_destination_owner");
    ERROR(rename(STICKY "/victim", STICKY "/owned"), EPERM, "sticky_rename_source_owner");
    CHECK(!rename(STICKY "/owned", STICKY "/free") &&
          !rename(STICKY "/free", STICKY "/owned"), "sticky_owned_source_new_name");
    return 0;
}
static int sticky_directory_rename(void)
{
    CHECK(!rename(STICKY "/victim", STICKY "/owned"), "sticky_directory_owner_replacement");
    return 0;
}
static int sticky_rules(void)
{
    CHECK(!mkdir(STICKY, 01777) && !chown(STICKY, 1001, 2001) && !chmod(STICKY, 01777) &&
          !create_file(STICKY "/victim", 1002, 2001, 0644), "sticky_setup");
    CHECK(!run_child(sticky_unlink_denied, 1004, 3000, (gid_t)-1) &&
          !run_child(sticky_unlink_owner, 1002, 3000, (gid_t)-1) &&
          !create_file(STICKY "/victim", 1002, 2001, 0644) &&
          !run_child(sticky_unlink_owner, 1001, 3000, (gid_t)-1), "sticky_unlink_rules");
    puts("PASS glibc_permissions_sticky_unlink_file_directory_owner");
    CHECK(!create_file(STICKY "/victim", 1002, 2001, 0644) &&
          !create_file(STICKY "/owned", 1003, 2001, 0644) &&
          !run_child(sticky_rename_denied, 1003, 3000, (gid_t)-1) &&
          !run_child(sticky_directory_rename, 1001, 3000, (gid_t)-1), "sticky_rename_rules");
    CHECK(!unlink(STICKY "/owned") && !rmdir(STICKY), "sticky_cleanup");
    puts("PASS glibc_permissions_sticky_rename_source_destination_replacement");
    return 0;
}
static int alias_metadata(void)
{
    struct stat before, a, b;
    CHECK(!stat(FILE, &before) && !chmod(ALIAS, 0644) && !chown(ALIAS, (uid_t)-1, 2002) &&
          !stat(FILE, &a) && !stat(ALIAS, &b) && a.st_ino == b.st_ino && a.st_nlink == 2 &&
          a.st_uid == 1001 && a.st_gid == 2002 && b.st_gid == 2002 &&
          (a.st_mode & 07777) == 0644 && (b.st_mode & 07777) == 0644 &&
          a.st_ctim.tv_sec >= before.st_ctim.tv_sec, "hardlink_shared_metadata_ctime_seconds");
    ERROR(chown(FILE, 1002, (gid_t)-1), EPERM, "owner_cannot_transfer_uid");
    ERROR(chown(FILE, (uid_t)-1, 2003), EPERM, "owner_cannot_choose_unrelated_group");
    CHECK(!stat(FILE, &b) && b.st_uid == a.st_uid && b.st_gid == a.st_gid &&
          b.st_mode == a.st_mode && b.st_ctim.tv_sec == a.st_ctim.tv_sec,
          "failed_chown_does_not_change_metadata");
    return 0;
}
static int unrelated_metadata(void)
{
    ERROR(chmod(FILE, 0777), EPERM, "nonowner_chmod");
    ERROR(chown(FILE, 1004, 3000), EPERM, "nonowner_chown");
    return 0;
}
static int ownership(void)
{
    CHECK(!create_file(FILE, 1001, 2001, 0640) && !link(FILE, ALIAS) &&
          !run_child(alias_metadata, 1001, 2001, 2002) &&
          !run_child(unrelated_metadata, 1004, 3000, (gid_t)-1), "ownership_children");
    struct stat a, b;
    CHECK(!chown(ALIAS, 70001, 70002) && !stat(FILE, &a) && !stat(ALIAS, &b) &&
          a.st_uid == 70001 && a.st_gid == 70002 && b.st_uid == 70001 && b.st_gid == 70002 &&
          !unlink(ALIAS) && !unlink(FILE), "root_chown_shared_wide_ids");
    puts("PASS glibc_permissions_hardlink_chmod_chown_ids_ctime_seconds");
    puts("PASS glibc_permissions_denied_chown_preserves_metadata");
    return 0;
}
static int inherited_group(void)
{
    mode_t previous = umask(0027);
    int fd = open(BASE "/setgid/file", O_CREAT | O_EXCL | O_RDWR, 0666);
    struct stat s;
    CHECK(fd >= 0 && !fstat(fd, &s) && s.st_uid == 1001 && s.st_gid == 2001 &&
          (s.st_mode & 07777) == 0640 && !close(fd), "setgid_file_group");
    CHECK(!mkdir(BASE "/setgid/sub", 0777) && !stat(BASE "/setgid/sub", &s) &&
          s.st_uid == 1001 && s.st_gid == 2001 && (s.st_mode & 07777) == 02750,
          "setgid_subdirectory_group_mode");
    CHECK(!rmdir(BASE "/setgid/sub") && !unlink(BASE "/setgid/file"), "setgid_child_cleanup");
    umask(previous);
    return 0;
}
static int setgid_directory(void)
{
    CHECK(!mkdir(BASE "/setgid", 0777) && !chown(BASE "/setgid", 0, 2001) &&
          !chmod(BASE "/setgid", 02777) && !run_child(inherited_group, 1001, 3000, (gid_t)-1) &&
          !rmdir(BASE "/setgid"), "setgid_directory");
    puts("PASS glibc_permissions_setgid_directory_group_umask_inheritance");
    return 0;
}

static int send_denied(void)
{
    CHECK(!drop(1004, 3000, (gid_t)-1), "signal_denied_ids");
    union sigval value = {.sival_int = 17};
    ERROR(kill(signal_target, 0), EPERM, "kill_zero_permission");
    ERROR(syscall(SYS_tgkill, signal_target, signal_target, 0), EPERM, "tgkill_zero_permission");
    ERROR(sigqueue(signal_target, 0, value), EPERM, "sigqueue_zero_permission");
    siginfo_t forged;
    memset(&forged, 0, sizeof(forged));
    forged.si_signo = SIGRTMIN;
    forged.si_code = SI_QUEUE;
    forged.si_pid = signal_target;
    forged.si_uid = 1001;
    ERROR(syscall(SYS_rt_sigqueueinfo, signal_target, SIGRTMIN, &forged), EPERM,
          "forged_siginfo_cannot_authorize");
    CHECK(!kill(signal_target, SIGCONT), "same_session_continue_exception");
    return 0;
}
static int send_effective_match(void)
{
    CHECK(!setgroups(0, NULL) && !setresgid(3000, 3000, 3000) && !setresuid(1004, 1001, 1004),
          "signal_effective_match_ids");
    CHECK(!kill(signal_target, 0) && !syscall(SYS_tgkill, signal_target, signal_target, 0),
          "signal_effective_matches_target_real");
    return 0;
}
static int send_effective_only_denied(void)
{
    CHECK(!setgroups(0, NULL) && !setresgid(3000, 3000, 3000) && !setresuid(1004, 1002, 1003),
          "signal_effective_only_ids");
    /* Target effective UID and sender saved UID do not authorize transmission. */
    union sigval value = {.sival_int = 19};
    ERROR(kill(signal_target, 0), EPERM, "target_effective_only_kill_denied");
    ERROR(syscall(SYS_tgkill, signal_target, signal_target, 0), EPERM, "target_effective_only_tgkill_denied");
    ERROR(sigqueue(signal_target, 0, value), EPERM, "sender_saved_only_sigqueue_denied");
    CHECK(!setresuid(1003, 1003, 1003) && !kill(signal_target, 0), "sender_saved_transition_then_allowed");
    return 0;
}
static int send_saved_match(void)
{
    CHECK(!setgroups(0, NULL) && !setresgid(3000, 3000, 3000) && !setresuid(1003, 1004, 1004),
          "signal_saved_match_ids");
    union sigval value = {.sival_int = 23};
    CHECK(!kill(signal_target, 0) && !syscall(SYS_tgkill, signal_target, signal_target, 0) &&
          !sigqueue(signal_target, 0, value), "signal_real_matches_target_saved");
    siginfo_t forged;
    memset(&forged, 0, sizeof(forged));
    forged.si_signo = SIGRTMIN;
    forged.si_code = SI_QUEUE;
    forged.si_pid = 1234567;
    forged.si_uid = 7654321;
    forged.si_value.sival_int = 0x31415;
    CHECK(!syscall(SYS_rt_sigqueueinfo, signal_target, SIGRTMIN, &forged), "queued_signal_send");
    /* A permitted later real-ID change must not rewrite already queued metadata. */
    CHECK(!setresuid(1004, 1004, 1004), "sender_change_real_after_queue");
    return 0;
}
static int send_cross_session(void)
{
    CHECK(setsid() >= 0 && !drop(1004, 3000, (gid_t)-1), "cross_session_ids");
    ERROR(kill(signal_target, SIGCONT), EPERM, "cross_session_continue_denied");
    return 0;
}
static int signal_sender(int (*test)(void), int read_fd, int write_fd, pid_t *sender_pid)
{
    pid_t sender = fork();
    if (!sender) {
        close(read_fd);
        int rc = test();
        if (!rc && put_byte(write_fd, 's')) rc = fail("sender_complete");
        _exit(rc);
    }
    CHECK(sender > 0, "sender_fork");
    int rc = get_byte(read_fd, 's');
    if (rc) kill(sender, SIGKILL);
    int status = reap(sender);
    CHECK(!rc && !status, "sender_result");
    if (sender_pid) *sender_pid = sender;
    return 0;
}
static int signal_receiver(int command_fd, int output_fd)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    sigaddset(&set, SIGCONT);
    CHECK(!sigprocmask(SIG_BLOCK, &set, NULL) && !setgroups(0, NULL) &&
          !setresgid(2001, 2002, 2003) && !setresuid(1001, 1002, 1003) &&
          !put_byte(output_fd, 'r'), "receiver_setup");
    /* Bound each parent/sender phase independently, rather than one total budget. */
    for (unsigned phase = 0; phase < 5; phase++)
        CHECK(!get_byte(command_fd, 'c') && !put_byte(output_fd, 'r'), "receiver_phase");
    CHECK(!get_byte(command_fd, 'q'), "receiver_release");
    pid_t sender;
    CHECK(read(command_fd, &sender, sizeof(sender)) == sizeof(sender), "receiver_expected_sender");
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    struct timespec timeout = {10, 0};
    siginfo_t info;
    CHECK(sigtimedwait(&set, &info, &timeout) == SIGRTMIN && info.si_signo == SIGRTMIN &&
          info.si_code == SI_QUEUE && info.si_uid == 1003 && info.si_pid == sender &&
          info.si_value.sival_int == 0x31415, "queued_actual_frozen_sender_identity");
    CHECK(!put_byte(output_fd, 'p'), "receiver_complete");
    return 0;
}
static int signal_permissions(void)
{
    int commands[2], replies[2];
    CHECK(!pipe(commands) && !pipe(replies), "signal_pipes");
    signal_target = fork();
    if (!signal_target) {
        close(commands[1]); close(replies[0]);
        _exit(signal_receiver(commands[0], replies[1]));
    }
    CHECK(signal_target > 0, "receiver_fork");
    close(commands[0]);
    int rc = get_byte(replies[0], 'r');
    pid_t sender = -1;
    union sigval value = {.sival_int = 0};
    if (!rc && (kill(signal_target, 0) || syscall(SYS_tgkill, signal_target, signal_target, 0) ||
                sigqueue(signal_target, 0, value))) rc = fail("root_signal_permissions");
    if (!rc) rc = signal_sender(send_denied, replies[0], replies[1], NULL);
    if (!rc && (put_byte(commands[1], 'c') || get_byte(replies[0], 'r'))) rc = fail("denied_checkpoint");
    if (!rc) puts("PASS glibc_permissions_signal_zero_identity_and_forgery");
    if (!rc) rc = signal_sender(send_effective_match, replies[0], replies[1], NULL);
    if (!rc && (put_byte(commands[1], 'c') || get_byte(replies[0], 'r'))) rc = fail("effective_checkpoint");
    if (!rc) rc = signal_sender(send_effective_only_denied, replies[0], replies[1], NULL);
    if (!rc && (put_byte(commands[1], 'c') || get_byte(replies[0], 'r'))) rc = fail("effective_only_checkpoint");
    if (!rc) rc = signal_sender(send_cross_session, replies[0], replies[1], NULL);
    if (!rc && (put_byte(commands[1], 'c') || get_byte(replies[0], 'r'))) rc = fail("session_checkpoint");
    if (!rc) puts("PASS glibc_permissions_signal_continue_session_scope");
    if (!rc) rc = signal_sender(send_saved_match, replies[0], replies[1], &sender);
    if (!rc && (put_byte(commands[1], 'c') || get_byte(replies[0], 'r'))) rc = fail("saved_checkpoint");
    if (!rc) puts("PASS glibc_permissions_signal_real_effective_saved_uid_matches");
    if (!rc && (put_byte(commands[1], 'q') || write(commands[1], &sender, sizeof(sender)) != sizeof(sender) ||
                get_byte(replies[0], 'p'))) rc = fail("receiver_metadata_result");
    if (rc) kill(signal_target, SIGKILL);
    int status = reap(signal_target);
    close(commands[1]); close(replies[0]); close(replies[1]);
    CHECK(!rc && !status, "signal_cleanup");
    puts("PASS glibc_permissions_siginfo_actual_real_uid_frozen");
    return 0;
}
int main(void)
{
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
    CHECK(!getresuid(&ruid, &euid, &suid) && !getresgid(&rgid, &egid, &sgid) &&
          !ruid && !euid && !suid && !rgid && !egid && !sgid, "initial_root_identity");
    mode_t previous = umask(0);
    CHECK(!mkdir(BASE, 0777), "base_directory");
    int rc;
    if ((rc = dac_modes()) || (rc = access_modes()) || (rc = descriptor_rights()) ||
        (rc = component_search()) || (rc = sticky_rules()) || (rc = ownership()) ||
        (rc = setgid_directory()) || (rc = signal_permissions())) return rc;
    CHECK(!rmdir(BASE), "base_cleanup");
    umask(previous);
    puts("PASS glibc_permissions_all");
    return 0;
}
