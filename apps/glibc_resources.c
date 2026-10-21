/* Copyright (C) 2026 NOS contributors. Real glibc process resource limits. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* These are NOS profile bounds, not a claim about POSIX minimum capacity. */
#define FD_CEILING 32u
#define STACK_BYTES (256u * 1024u)
#define DATA "/glibc-resources-data"
#define TARGET "/glibc-resources-target"
#define CHECK(condition, step) do { if (!(condition)) return fail(step); } while (0)
#define ERROR(operation, code, step) do { errno = 0; CHECK((operation) == -1 && errno == (code), step); } while (0)

struct limit_pair { uint64_t soft, hard; };
_Static_assert(sizeof(struct limit_pair) == 16, "prlimit64 wire size");

static int fail(const char *step)
{
    fprintf(stderr, "FAIL glibc_resources %s errno=%d\n", step, errno);
    return 97;
}

static void deadline(int signal)
{
    static const char message[] = "FAIL glibc_resources child_deadline_10s\n";
    (void)signal;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(98);
}

static int arm_deadline(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = deadline;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return fail("deadline_handler");
    alarm(10);
    return 0;
}

static int reap(pid_t child, const char *step)
{
    int status;
    pid_t result;
    do result = waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
    if (result == child && WIFEXITED(status) && !WEXITSTATUS(status)) return 0;
    if (result == child) fprintf(stderr, "resources child %s status=%d\n", step, status);
    return fail(step);
}

static int isolated(int (*test)(void), const char *step)
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        if (arm_deadline()) _exit(97);
        _exit(test());
    }
    return child < 0 ? fail("isolated_fork") : reap(child, step);
}

static int raw_limit(const void *replacement, void *old)
{
    return (int)syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, replacement, old);
}

static int set_limit(uint64_t soft, uint64_t hard)
{
    struct rlimit64 value = {(rlim64_t)soft, (rlim64_t)hard};
    return setrlimit64(RLIMIT_NOFILE, &value);
}

static int limits_are(uint64_t soft, uint64_t hard)
{
    struct rlimit narrow;
    struct rlimit64 wide, process;
    struct limit_pair raw;
    return !getrlimit(RLIMIT_NOFILE, &narrow) && !getrlimit64(RLIMIT_NOFILE, &wide) &&
        !prlimit64(getpid(), RLIMIT_NOFILE, NULL, &process) && !raw_limit(NULL, &raw) &&
        (uint64_t)narrow.rlim_cur == soft && (uint64_t)narrow.rlim_max == hard &&
        wide.rlim_cur == soft && wide.rlim_max == hard &&
        process.rlim_cur == soft && process.rlim_max == hard &&
        raw.soft == soft && raw.hard == hard &&
        sysconf(_SC_OPEN_MAX) == (long)soft && getdtablesize() == (int)soft;
}

static int query_routes(void)
{
    CHECK(limits_are(FD_CEILING, FD_CEILING), "initial_libc_queries_match_actual_bound");
    struct rlimit old;
    CHECK(!prlimit(0, RLIMIT_NOFILE, NULL, &old) && old.rlim_cur == FD_CEILING &&
          old.rlim_max == FD_CEILING, "prlimit_native_query");
    struct rlimit replacement = {18, FD_CEILING};
    CHECK(!setrlimit(RLIMIT_NOFILE, &replacement) && limits_are(18, FD_CEILING),
          "setrlimit_native_changes_all_query_routes");
    replacement.rlim_cur = 17;
    CHECK(!prlimit(getpid(), RLIMIT_NOFILE, &replacement, &old) &&
          old.rlim_cur == 18 && old.rlim_max == FD_CEILING && limits_are(17, FD_CEILING),
          "prlimit_native_old_value");
    CHECK(!raw_limit(NULL, NULL) && limits_are(17, FD_CEILING), "prlimit_both_pointers_null_is_valid");
#if defined(__arm__)
    uint32_t arm_pair[2] = {0, 0};
    CHECK(!syscall(191, RLIMIT_NOFILE, arm_pair) && arm_pair[0] == 17 && arm_pair[1] == FD_CEILING,
          "arm_ugetrlimit_unsigned32_layout");
    ERROR(syscall(191, RLIMIT_NOFILE, NULL), EFAULT, "arm_ugetrlimit_null_output");
#endif
    return 0;
}

static int atomic_errors(void)
{
    ERROR(set_limit(33, 32), EINVAL, "soft_above_hard");
    CHECK(limits_are(32, 32), "invalid_order_unchanged");
    ERROR(set_limit(32, 33), EPERM, "hard_above_structural_ceiling");
    CHECK(limits_are(32, 32), "ceiling_failure_unchanged");
    ERROR(set_limit(UINT64_MAX, UINT64_MAX), EPERM, "infinity_not_fake_capacity");
    ERROR(set_limit(UINT64_MAX, 32), EINVAL, "full_width_soft_order");
    CHECK(limits_are(32, 32), "wide_failure_unchanged");
    CHECK(!set_limit(10, 20) && limits_are(10, 20) &&
          !set_limit(32, 32) && limits_are(32, 32), "root_can_restore_lower_hard_within_ceiling");
    struct limit_pair replacement = {19, 27}, old = {0, 0};
    CHECK(!raw_limit(&replacement, &old) && old.soft == 32 && old.hard == 32 &&
          limits_are(19, 27), "prlimit_returns_prechange_pair");
    return 0;
}

static int pointer_rollback(void)
{
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0, "page_size");
    unsigned char *memory = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED && !mprotect(memory + page, (size_t)page, PROT_NONE),
          "guard_mapping");
    struct limit_pair replacement = {7, 20}, old = {0x1234, 0x5678};
    ERROR(raw_limit(&replacement, (void *)(uintptr_t)1), EFAULT, "bad_old_pointer");
    CHECK(limits_are(32, 32), "bad_old_preserves_limit");
    ERROR(raw_limit((void *)(uintptr_t)1, &old), EFAULT, "bad_new_pointer");
    CHECK(old.soft == 0x1234 && old.hard == 0x5678 && limits_are(32, 32),
          "bad_new_preserves_old_output_and_limit");
    uint64_t *edge = (uint64_t *)(void *)(memory + page - sizeof(uint64_t));
    *edge = UINT64_C(0x123456789abcdef0);
    ERROR(raw_limit(&replacement, edge), EFAULT, "old_crosses_guard");
    CHECK(*edge == UINT64_C(0x123456789abcdef0) && limits_are(32, 32),
          "guard_old_preflight_no_partial_output_or_change");
    *edge = 7;
    ERROR(raw_limit(edge, &old), EFAULT, "new_crosses_guard");
    CHECK(old.soft == 0x1234 && old.hard == 0x5678 && limits_are(32, 32),
          "guard_new_atomic_failure");
    struct limit_pair *readonly = (struct limit_pair *)(void *)memory;
    readonly->soft = 0x1111; readonly->hard = 0x2222;
    CHECK(!mprotect(memory, (size_t)page, PROT_READ), "readonly_output_mapping");
    ERROR(raw_limit(&replacement, readonly), EFAULT, "readonly_old_pointer");
    CHECK(readonly->soft == 0x1111 && readonly->hard == 0x2222 && limits_are(32, 32),
          "readonly_output_and_state_unchanged");
    CHECK(!mprotect(memory, (size_t)page, PROT_READ | PROT_WRITE), "restore_output_mapping");
    struct limit_pair alias = {17, 24};
    CHECK(!raw_limit(&alias, &alias) && alias.soft == 32 && alias.hard == 32 &&
          limits_are(17, 24), "aliased_new_old_uses_complete_candidate");
    CHECK(!munmap(memory, (size_t)page * 2), "release_guard_mapping");
    return 0;
}

static void close_extra_fds(void)
{
    for (unsigned fd = 3; fd < FD_CEILING; fd++) (void)close((int)fd);
}

static int lock_survives_child(int fd, pid_t owner)
{
    struct flock query;
    memset(&query, 0, sizeof(query));
    query.l_type = F_WRLCK; query.l_whence = SEEK_SET; query.l_len = 1;
    CHECK(!fcntl(fd, F_GETLK, &query) && query.l_type == F_WRLCK && query.l_pid == owner,
          "rejected_dup2_does_not_release_target_record_lock");
    return 0;
}

static int open_dup_boundaries(void)
{
    close_extra_fds();
    int source = open(DATA, O_RDWR), target = open(TARGET, O_RDWR);
    CHECK(source == 3 && target == 4, "standard_descriptors_count_towards_limit");
    CHECK(dup2(target, 12) == 12 && !close(target), "prepare_existing_high_descriptor");
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_len = 1;
    CHECK(!fcntl(12, F_SETLK, &lock) && !set_limit(4, 32), "lock_then_lower_below_existing_fd");
    ERROR(open(DATA, O_RDONLY), EMFILE, "open_rejects_first_out_of_limit_fd");
    ERROR(dup(source), EMFILE, "dup_has_no_permitted_hole");
    ERROR(fcntl(source, F_DUPFD, 4), EINVAL, "dupfd_minimum_at_limit");
    ERROR(fcntl(source, F_DUPFD_CLOEXEC, 4), EINVAL, "dupfd_cloexec_minimum_at_limit");
    ERROR(dup2(source, 4), EBADF, "dup2_target_at_limit");
    ERROR(dup3(source, 4, O_CLOEXEC), EBADF, "dup3_target_at_limit");
    ERROR(dup2(source, -1), EBADF, "negative_dup2_target_not_allocation_mode");
    ERROR(dup2(source, -2), EBADF, "negative_dup2_target_not_plain_dup_mode");
    ERROR(dup3(source, -1, O_CLOEXEC), EBADF, "negative_dup3_target");
    ERROR(dup2(source, 12), EBADF, "dup2_existing_out_of_limit_target_unchanged");
    CHECK(dup2(12, 12) == 12, "same_fd_dup2_valid_above_lowered_limit");
    ERROR(dup3(12, 12, O_CLOEXEC), EINVAL, "same_fd_dup3_invalid");
    struct stat status;
    struct pollfd pollfd = {12, POLLIN, 0};
    char byte = 0;
    CHECK(!fstat(12, &status) && fcntl(12, F_GETFD) == 0 && poll(&pollfd, 1, 0) == 1 &&
          (pollfd.revents & POLLIN) && pread(12, &byte, 1, 0) == 1 && byte == 'T',
          "old_high_descriptor_metadata_poll_and_contents_survive");
    pid_t owner = getpid(), child = fork();
    if (!child) _exit(lock_survives_child(12, owner));
    CHECK(child > 0 && !reap(child, "record_lock_observer"), "failed_dup2_preserves_lock");
    CHECK(pwrite(12, "Z", 1, 0) == 1 && pread(12, &byte, 1, 0) == 1 && byte == 'Z',
          "old_high_descriptor_writable_after_lowering");
    CHECK(!close(source), "free_permitted_hole");
    int reopened = open(DATA, O_RDONLY);
    CHECK(reopened == 3 && limits_are(4, 32), "open_reuses_lowest_permitted_hole");
    CHECK(!set_limit(0, 32) && limits_are(0, 32), "zero_soft_limit_query");
    ERROR(open(DATA, O_RDONLY), EMFILE, "zero_limit_open");
    ERROR(dup(reopened), EMFILE, "zero_limit_dup");
    ERROR(fcntl(reopened, F_DUPFD, 0), EINVAL, "zero_limit_dupfd_minimum");
    CHECK(dup2(12, 12) == 12 && pread(12, &byte, 1, 0) == 1 && byte == 'Z' &&
          !close(12) && !close(reopened), "zero_limit_preserves_existing_fds_and_close");
    return 0;
}

static int pipe_pair_atomicity(void)
{
    close_extra_fds();
    int source = open(DATA, O_RDONLY);
    CHECK(source == 3 && !set_limit(5, 32), "one_permitted_hole_for_pipe");
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int pair[2] = {0x1234, 0x5678};
        ERROR(pipe2(pair, O_CLOEXEC | O_NONBLOCK), EMFILE, "pipe_pair_requires_two_permitted_fds");
        CHECK(pair[0] == 0x1234 && pair[1] == 0x5678, "failed_pipe_keeps_output_pair");
    }
    int last = open(DATA, O_RDONLY);
    CHECK(last == 4 && !close(last) && !close(source), "failed_pipe_reserves_no_fd");
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int pair[2];
        CHECK(!pipe2(pair, O_CLOEXEC | O_NONBLOCK) && pair[0] == 3 && pair[1] == 4,
              "pipe_succeeds_after_two_holes_exist");
        CHECK(fcntl(pair[0], F_GETFD) == FD_CLOEXEC && fcntl(pair[1], F_GETFD) == FD_CLOEXEC &&
              (fcntl(pair[0], F_GETFL) & O_NONBLOCK) && (fcntl(pair[1], F_GETFL) & O_NONBLOCK),
              "pipe_flags_survive_atomic_admission");
        char byte = 0;
        CHECK(write(pair[1], "p", 1) == 1 && read(pair[0], &byte, 1) == 1 && byte == 'p' &&
              !close(pair[0]) && !close(pair[1]), "pipe_resources_recover_after_close");
    }
    CHECK(!set_limit(0, 32), "zero_limit_pipe_setup");
    int pair[2] = {0x1234, 0x5678};
    ERROR(pipe(pair), EMFILE, "zero_limit_pipe");
    CHECK(pair[0] == 0x1234 && pair[1] == 0x5678, "zero_limit_pipe_no_output_change");
    return 0;
}

static pthread_barrier_t shared_barrier;
static int shared_result;

static int barrier(void)
{
    int rc = pthread_barrier_wait(&shared_barrier);
    return !rc || rc == PTHREAD_BARRIER_SERIAL_THREAD ? 0 : rc;
}

static void *shared_worker(void *argument)
{
    (void)argument;
    if (!limits_are(12, 24) || set_limit(9, 20)) shared_result = 1;
    if (barrier()) shared_result = 2;
    if (barrier()) shared_result = 3;
    if (!limits_are(8, 20)) shared_result = 4;
    return NULL;
}

static int thread_shared_fork_independent(void)
{
    CHECK(!set_limit(12, 24) && !pthread_barrier_init(&shared_barrier, NULL, 2), "thread_limit_setup");
    pthread_t worker;
    int rc = pthread_create(&worker, NULL, shared_worker, NULL);
    CHECK(!rc, "create_limit_worker");
    CHECK(!barrier() && limits_are(9, 20) && !set_limit(8, 20) && !barrier(),
          "both_threads_observe_same_mutable_process_limits");
    CHECK(!pthread_join(worker, NULL) && !shared_result && !pthread_barrier_destroy(&shared_barrier),
          "join_limit_worker");
    pid_t child = fork();
    if (!child) _exit(limits_are(8, 20) && !set_limit(5, 15) && limits_are(5, 15) ? 0 :
                      fail("fork_inherits_then_changes_independent_limit_object"));
    CHECK(child > 0 && !reap(child, "fork_limit_child") && limits_are(8, 20),
          "fork_limit_change_does_not_modify_parent");
    return 0;
}

static const char *program_path;

static int exec_stage(int argc, char **argv)
{
    CHECK(argc == 5 && getpid() == (pid_t)strtol(argv[2], NULL, 10), "exec_stage_pid_arguments");
    uint64_t soft = strtoull(argv[3], NULL, 10), hard = strtoull(argv[4], NULL, 10);
    CHECK(limits_are(soft, hard), "exec_preserves_mutable_process_limit_object");
    CHECK(!set_limit(9, hard) && limits_are(9, hard), "post_exec_limit_still_mutable");
    return 0;
}

static void *exec_worker(void *argument)
{
    char **arguments = argument;
    extern char **environ;
    if (set_limit(12, 24)) _exit(fail("nonleader_limit_before_exec"));
    execve(arguments[0], arguments, environ);
    _exit(fail("nonleader_execve"));
}

static int exec_child(int nonleader)
{
    CHECK(!set_limit(12, 24), "prepare_exec_limits");
    extern char **environ;
    char *missing[] = {"/resources-no-such-executable", NULL};
    ERROR(execve(missing[0], missing, environ), ENOENT, "failed_exec_missing");
    CHECK(limits_are(12, 24), "missing_exec_preserves_limits");
    volatile uintptr_t bad = 1;
    ERROR(execve(program_path, (char *const *)bad, environ), EFAULT, "failed_exec_bad_argv");
    CHECK(limits_are(12, 24), "faulting_exec_preserves_limits");
    char pid[16];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    char *arguments[] = {(char *)program_path, "exec-limits", pid, "12", "24", NULL};
    if (nonleader) {
        CHECK(!set_limit(13, 25), "prepare_nonleader_shared_limit");
        pthread_t worker;
        int rc = pthread_create(&worker, NULL, exec_worker, arguments);
        CHECK(!rc, "create_exec_worker");
        rc = pthread_join(worker, NULL);
        errno = rc;
        return fail("nonleader_exec_returned");
    }
    execve(program_path, arguments, environ);
    return fail("leader_execve");
}

static int leader_exec_child(void) { return exec_child(0); }
static int nonleader_exec_child(void) { return exec_child(1); }

static void *after_leader_exit(void *argument)
{
    pthread_t leader = *(pthread_t *)argument;
    void *value = NULL;
    int rc = pthread_join(leader, &value);
    if (rc || value != (void *)(uintptr_t)0x51 || !limits_are(12, 24) ||
        set_limit(9, 20) || !limits_are(9, 20)) _exit(fail("leader_exit_preserves_shared_limits"));
    pid_t child = fork();
    if (!child) _exit(limits_are(9, 20) ? 0 : fail("leader_exit_worker_fork_limits"));
    _exit(child < 0 ? fail("leader_exit_worker_fork") : reap(child, "leader_exit_worker_child"));
}

static int leader_exit_child(void)
{
    CHECK(!set_limit(12, 24), "leader_exit_limit_setup");
    static pthread_t leader;
    leader = pthread_self();
    pthread_t worker;
    int rc = pthread_create(&worker, NULL, after_leader_exit, &leader);
    CHECK(!rc, "create_leader_exit_worker");
    pthread_exit((void *)(uintptr_t)0x51);
}

static int unprivileged_hard_raise(void)
{
    CHECK(!set_limit(8, 16) && !setuid((uid_t)70001), "prepare_unprivileged_limit");
    ERROR(set_limit(8, 17), EPERM, "unprivileged_raise_hard_denied");
    CHECK(limits_are(8, 16), "hard_raise_denial_unchanged");
    ERROR(set_limit(17, 16), EINVAL, "unprivileged_soft_above_hard");
    CHECK(!set_limit(7, 12) && limits_are(7, 12) &&
          !set_limit(12, 12) && limits_are(12, 12), "unprivileged_lower_hard_raise_soft_within_hard");
    ERROR(set_limit(12, 13), EPERM, "unprivileged_cannot_restore_discarded_hard");
    CHECK(limits_are(12, 12) && !set_limit(0, 12) && limits_are(0, 12),
          "unprivileged_zero_soft_limit");
    return 0;
}

static int stack_unsupported(void)
{
    struct rlimit stack;
    struct rlimit64 stack64;
    CHECK(!getrlimit(RLIMIT_STACK, &stack) && !getrlimit64(RLIMIT_STACK, &stack64) &&
          stack.rlim_cur == STACK_BYTES && stack.rlim_max == STACK_BYTES &&
          stack64.rlim_cur == STACK_BYTES && stack64.rlim_max == STACK_BYTES,
          "stack_query_routes_agree");
    CHECK(!setrlimit(RLIMIT_STACK, &stack), "stack_equal_value_supported");
    stack64.rlim_cur = STACK_BYTES / 2;
    ERROR(setrlimit64(RLIMIT_STACK, &stack64), ENOTSUP, "stack_mutation_explicitly_unsupported");
    CHECK(!getrlimit64(RLIMIT_STACK, &stack64) && stack64.rlim_cur == STACK_BYTES &&
          stack64.rlim_max == STACK_BYTES, "unsupported_stack_mutation_unchanged");
    struct rlimit64 unsupported = {0x1234, 0x5678};
    ERROR(getrlimit64(RLIMIT_NPROC, &unsupported), ENOTSUP, "process_capacity_not_fake_unlimited");
    CHECK(unsupported.rlim_cur == 0x1234 && unsupported.rlim_max == 0x5678,
          "unsupported_resource_output_unchanged");
    ERROR(syscall(SYS_prlimit64, 0, 16, NULL, &unsupported), EINVAL,
          "resource_number_outside_linux_table");
    ERROR(syscall(SYS_prlimit64, 0, -1, NULL, &unsupported), EINVAL,
          "negative_resource_number");
    CHECK(unsupported.rlim_cur == 0x1234 && unsupported.rlim_max == 0x5678 &&
          limits_are(32, 32), "invalid_resource_keeps_output_and_supported_limits");
    return 0;
}

static int create_data(const char *path, const char *bytes, size_t count)
{
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, bytes, count) == (ssize_t)count && !close(fd), "create_test_data");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exec-limits")) {
        if (arm_deadline()) return 97;
        return exec_stage(argc, argv);
    }
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "qemu")), "arguments");
    CHECK(limits_are(FD_CEILING, FD_CEILING), "orchestrator_initial_limits");
    program_path = argv[0];
    CHECK(!create_data(DATA, "Source", 6) && !create_data(TARGET, "Target", 6), "test_file_setup");
    CHECK(!isolated(query_routes, "query_routes"), "query_routes_child");
    puts("PASS glibc_resources_query_routes");
#if defined(__arm__)
    puts("PASS glibc_resources_arm_ugetrlimit");
#endif
    CHECK(!isolated(atomic_errors, "atomic_errors"), "atomic_errors_child");
    puts("PASS glibc_resources_nofile_atomic_errors");
    CHECK(!isolated(pointer_rollback, "pointer_rollback"), "pointer_rollback_child");
    puts("PASS glibc_resources_pointer_rollback");
    CHECK(!isolated(open_dup_boundaries, "open_dup_boundaries"), "open_dup_child");
    puts("PASS glibc_resources_open_dup_boundaries");
    CHECK(!isolated(pipe_pair_atomicity, "pipe_pair_atomicity"), "pipe_pair_child");
    puts("PASS glibc_resources_pipe_pair_atomicity");
    CHECK(!isolated(thread_shared_fork_independent, "thread_shared_fork_independent"), "thread_fork_child");
    puts("PASS glibc_resources_thread_shared_fork_independent");
    CHECK(!isolated(leader_exec_child, "leader_exec"), "leader_exec_child");
    puts("PASS glibc_resources_exec_preserves_limits");
    CHECK(!isolated(nonleader_exec_child, "nonleader_exec"), "nonleader_exec_child");
    puts("PASS glibc_resources_nonleader_exec");
    CHECK(!isolated(leader_exit_child, "leader_exit"), "leader_exit_child");
    puts("PASS glibc_resources_leader_exit");
    CHECK(!isolated(unprivileged_hard_raise, "unprivileged_hard_raise"), "unprivileged_child");
    puts("PASS glibc_resources_unprivileged_hard_raise");
    CHECK(!isolated(stack_unsupported, "stack_unsupported"), "stack_unsupported_child");
    puts("PASS glibc_resources_stack_unsupported");
    CHECK(limits_are(FD_CEILING, FD_CEILING) && !unlink(DATA) && !unlink(TARGET),
          "orchestrator_unchanged_and_cleanup");
    puts("PASS glibc_posix_resources");
    return 0;
}
