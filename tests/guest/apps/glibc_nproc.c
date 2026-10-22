/* Copyright (C) 2026 NOS contributors. Real glibc per-UID task admission. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SELF "/bin/glibc_nproc"
#define UID_THREADS ((uid_t)73121)
#define UID_HARD ((uid_t)73122)
#define UID_EXEC ((uid_t)73123)
#define UID_TRANSITION ((uid_t)73124)
#define UID_LOWER ((uid_t)73125)
#define UID_EFFECTIVE ((uid_t)73126)
#define CHECK(condition, step) do { if (!(condition)) return fail(step); } while (0)
#define ERROR(operation, code, step) do { errno = 0; CHECK((operation) == -1 && errno == (code), step); } while (0)

struct limit_pair { uint64_t soft, hard; };
_Static_assert(sizeof(struct limit_pair) == 16, "Linux prlimit64 pair");
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
struct worker_state { unsigned change, ready, release; int error; };
static struct worker_state workers[2];
static sig_atomic_t child_exits;
_Static_assert(__atomic_always_lock_free(sizeof(child_exits), 0), "signal-safe exit counter");

static int fail(const char *step)
{
    fprintf(stderr, "FAIL glibc_nproc %s errno=%d\n", step, errno);
    return 97;
}
static void deadline(int signal)
{
    static const char text[] = "FAIL glibc_nproc deadline\n";
    (void)signal;
    (void)write(STDERR_FILENO, text, sizeof(text) - 1);
    _exit(98);
}
static int set_deadline(unsigned seconds)
{
    struct sigaction action = {.sa_handler = deadline};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return fail("deadline_handler");
    alarm(seconds);
    return 0;
}
static int reap(pid_t child, const char *step)
{
    int status;
    pid_t result;
    do result = waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
    if (result == child && WIFEXITED(status) && !WEXITSTATUS(status)) return 0;
    if (result == child) fprintf(stderr, "nproc child %s status=%d\n", step, status);
    return fail(step);
}
static int isolated(int (*test)(void), const char *step)
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        int result = set_deadline(20);
        if (!result) result = test();
        fflush(NULL);
        _exit(result);
    }
    return child < 0 ? fail("isolated_fork") : reap(child, step);
}
static int raw_limit(const void *next, void *old)
{ return (int)syscall(SYS_prlimit64, 0, RLIMIT_NPROC, next, old); }
static int set_limit(uint64_t soft, uint64_t hard)
{
    struct rlimit64 limit = {(rlim64_t)soft, (rlim64_t)hard};
    return setrlimit64(RLIMIT_NPROC, &limit);
}
static int limits_are(uint64_t soft, uint64_t hard)
{
    struct rlimit narrow;
    struct rlimit64 wide, process;
    struct limit_pair raw;
    return !getrlimit(RLIMIT_NPROC, &narrow) && !getrlimit64(RLIMIT_NPROC, &wide) &&
        !prlimit64(getpid(), RLIMIT_NPROC, NULL, &process) && !raw_limit(NULL, &raw) &&
        narrow.rlim_cur == (soft == UINT64_MAX ? RLIM_INFINITY : (rlim_t)soft) &&
        narrow.rlim_max == (hard == UINT64_MAX ? RLIM_INFINITY : (rlim_t)hard) &&
        wide.rlim_cur == soft && wide.rlim_max == hard &&
        process.rlim_cur == soft && process.rlim_max == hard && raw.soft == soft && raw.hard == hard &&
        (soft == UINT64_MAX || sysconf(_SC_CHILD_MAX) == (long)soft);
}
static int queries_and_rollback(void)
{
    struct rlimit64 initial;
    CHECK(!getrlimit64(RLIMIT_NPROC, &initial) && initial.rlim_cur == 80 && initial.rlim_max == 80 &&
          limits_are(initial.rlim_cur, initial.rlim_max),
          "initial_query_routes");
    CHECK(!set_limit(UINT64_MAX, UINT64_MAX) && limits_are(UINT64_MAX, UINT64_MAX),
          "infinity_policy_keeps_structural_bound_separate");
    CHECK(!set_limit(6, 12) && limits_are(6, 12), "libc_set_and_query");
    ERROR(set_limit(13, 12), EINVAL, "soft_above_hard");
    CHECK(limits_are(6, 12) && !raw_limit(NULL, NULL), "invalid_order_preserves_limits");
    puts("PASS glibc_nproc_query_routes");
    struct limit_pair replacement = {4, 10}, old = {0x1234, 0x5678};
    ERROR(raw_limit(&replacement, (void *)(uintptr_t)1), EFAULT, "bad_old_pointer");
    CHECK(limits_are(6, 12), "bad_old_rollback");
    ERROR(raw_limit((void *)(uintptr_t)1, &old), EFAULT, "bad_new_pointer");
    CHECK(old.soft == 0x1234 && old.hard == 0x5678 && limits_are(6, 12), "bad_new_rollback");
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0, "page_size");
    unsigned char *guard = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED && !mprotect(guard + page, (size_t)page, PROT_NONE), "guard_map");
    uint64_t *edge = (uint64_t *)(void *)(guard + page - sizeof(uint64_t));
    *edge = UINT64_C(0x123456789abcdef0);
    ERROR(raw_limit(&replacement, edge), EFAULT, "old_crosses_guard");
    CHECK(*edge == UINT64_C(0x123456789abcdef0) && limits_are(6, 12), "old_guard_no_partial_write");
    *edge = 4;
    ERROR(raw_limit(edge, &old), EFAULT, "new_crosses_guard");
    CHECK(old.soft == 0x1234 && old.hard == 0x5678 && limits_are(6, 12), "new_guard_no_commit");
    struct limit_pair alias = {5, 11};
    CHECK(!raw_limit(&alias, &alias) && alias.soft == 6 && alias.hard == 12 && limits_are(5, 11),
          "new_old_alias_complete_candidate");
    CHECK(!munmap(guard, (size_t)page * 2), "guard_unmap");
    puts("PASS glibc_nproc_atomic_pointer_rollback");
    return 0;
}
static void *immediate(void *argument)
{ return argument; }
static int root_exemption(void)
{
    CHECK(getuid() == 0 && !set_limit(0, 0) && limits_are(0, 0), "root_zero_limit");
    pid_t child = fork();
    if (!child) _exit(getuid() || !limits_are(0, 0) ? 97 : 0);
    CHECK(child > 0 && !reap(child, "root_fork"), "root_fork_exempt");
    pthread_t thread;
    void *value = NULL;
    CHECK(!pthread_create(&thread, NULL, immediate, (void *)1) &&
          !pthread_join(thread, &value) && value == (void *)1, "root_thread_exempt");
    puts("PASS glibc_nproc_root_exemption");
    return 0;
}
static int hard_raise(void)
{
    CHECK(!set_limit(1, 2) && !setresuid(UID_HARD, UID_HARD, UID_HARD), "nonroot_setup");
    ERROR(set_limit(1, 3), EPERM, "nonroot_hard_raise");
    CHECK(limits_are(1, 2) && !set_limit(2, 2) && limits_are(2, 2) &&
          !set_limit(0, 1) && limits_are(0, 1), "nonroot_soft_and_hard_lowering");
    ERROR(set_limit(0, 2), EPERM, "nonroot_restore_hard_rejected");
    CHECK(limits_are(0, 1), "hard_raise_failure_preserves_pair");
    puts("PASS glibc_nproc_unprivileged_hard_raise");
    return 0;
}
static void *worker(void *argument)
{
    struct worker_state *state = argument;
    if (state->change && set_limit(3, 8)) state->error = errno ? errno : 1;
    int error = pthread_mutex_lock(&mutex);
    if (error) return (void *)(uintptr_t)error;
    state->ready = 1;
    pthread_cond_broadcast(&condition);
    while (!state->release) {
        error = pthread_cond_wait(&condition, &mutex);
        if (error) break;
    }
    pthread_mutex_unlock(&mutex);
    return (void *)(uintptr_t)(error ? error : state->error);
}
static int start_worker(pthread_t *thread, unsigned index, unsigned change)
{
    workers[index] = (struct worker_state){.change = change};
    int error = pthread_create(thread, NULL, worker, &workers[index]);
    if (error) { errno = error; return fail("worker_create"); }
    CHECK(!pthread_mutex_lock(&mutex), "worker_ready_lock");
    struct timespec deadline_time;
    CHECK(!clock_gettime(CLOCK_REALTIME, &deadline_time), "worker_ready_clock");
    deadline_time.tv_sec += 5;
    while (!workers[index].ready) {
        error = pthread_cond_timedwait(&condition, &mutex, &deadline_time);
        if (error) { errno = error; return fail("worker_ready_wait"); }
    }
    error = workers[index].error;
    CHECK(!pthread_mutex_unlock(&mutex), "worker_ready_unlock");
    CHECK(!error, "worker_limit_change");
    return 0;
}
static int finish_worker(pthread_t thread, unsigned index)
{
    CHECK(!pthread_mutex_lock(&mutex), "worker_release_lock");
    workers[index].release = 1;
    CHECK(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&mutex), "worker_release");
    void *value = (void *)1;
    CHECK(!pthread_join(thread, &value) && !value, "worker_join");
    return 0;
}
static int fork_rejected(const char *step)
{
    errno = 0;
    pid_t child = fork();
    int saved = errno;
    if (!child) _exit(97);
    if (child > 0) {
        (void)kill(child, SIGKILL);
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    errno = saved;
    return child == -1 && saved == EAGAIN ? 0 : fail(step);
}
static int thread_rejected(const char *step)
{
    pthread_t thread;
    int error = pthread_create(&thread, NULL, immediate, NULL);
    if (!error) (void)pthread_join(thread, NULL);
    if (error == EAGAIN) return 0;
    errno = error;
    return fail(step);
}
static int put_byte(int fd, char byte)
{
    ssize_t result;
    do result = write(fd, &byte, 1); while (result < 0 && errno == EINTR);
    return result == 1 ? 0 : -1;
}
static int get_byte(int fd, char expected)
{
    char byte = 0;
    ssize_t result;
    do result = read(fd, &byte, 1); while (result < 0 && errno == EINTR);
    return result == 1 && byte == expected ? 0 : -1;
}
static void child_notice(int signal)
{
    if (signal == SIGCHLD) __atomic_add_fetch(&child_exits, 1, __ATOMIC_RELAXED);
}
static int wait_notice(sig_atomic_t before)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    while (__atomic_load_n(&child_exits, __ATOMIC_RELAXED) == before) {
        sched_yield();
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - start.tv_sec >= 5) return -1;
    }
    return 0;
}
static int task_accounting(void)
{
    int command[2], report[2];
    CHECK(!pipe(command) && !pipe(report) && !set_limit(4, 8) &&
          !setresuid(UID_THREADS, UID_THREADS, UID_THREADS), "accounting_setup");
    struct sigaction action = {.sa_handler = child_notice};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGCHLD, &action, NULL), "sigchld_handler");
    pthread_t first, second;
    CHECK(!start_worker(&first, 0, 1) && limits_are(3, 8), "thread_shared_limit");
    pid_t helper = fork();
    if (!helper) {
        close(command[1]); close(report[0]);
        if (!limits_are(3, 8) || put_byte(report[1], 'R') || get_byte(command[0], 'F') ||
            fork_rejected("child_copied_limit_global_uid_count") || put_byte(report[1], 'E') ||
            get_byte(command[0], 'X')) _exit(97);
        _exit(0);
    }
    CHECK(helper > 0 && !close(command[0]) && !close(report[1]) && !get_byte(report[0], 'R'),
          "helper_ready_at_shared_uid_limit");
    CHECK(!fork_rejected("thread_and_process_hit_limit") &&
          !thread_rejected("clone_counted_at_shared_uid_limit"), "creation_at_limit");
    CHECK(!set_limit(4, 8) && !start_worker(&second, 1, 0), "raise_parent_soft_then_add_thread");
    CHECK(!put_byte(command[1], 'F') && !get_byte(report[0], 'E') && limits_are(4, 8),
          "fork_copy_independent_limits_same_uid_budget");
    CHECK(!fork_rejected("global_uid_four_tasks") && !thread_rejected("global_uid_four_clone"),
          "global_real_uid_budget");
    CHECK(!finish_worker(second, 1), "release_counted_thread");
    sig_atomic_t before = __atomic_load_n(&child_exits, __ATOMIC_RELAXED);
    pid_t zombie = fork();
    if (!zombie) _exit(0);
    CHECK(zombie > 0 && !wait_notice(before), "child_exited_without_reap");
    for (unsigned i = 0; i < 16; i++) CHECK(!fork_rejected("zombie_still_counted"), "no_admission_leak");
    CHECK(!thread_rejected("zombie_blocks_clone") && !reap(zombie, "zombie_reap"), "zombie_reap_releases_count");
    pid_t recovered = fork();
    if (!recovered) _exit(0);
    CHECK(recovered > 0 && !reap(recovered, "fork_after_reap"), "recovered_admission");
    CHECK(!put_byte(command[1], 'X') && !reap(helper, "budget_helper") &&
          !close(command[1]) && !close(report[0]) && !finish_worker(first, 0), "accounting_cleanup");
    int status;
    ERROR(waitpid(-1, &status, WNOHANG), ECHILD, "no_phantom_children_after_rejections");
    CHECK(!set_limit(1, 8) && !fork_rejected("one_remaining_task_at_limit") &&
          !set_limit(2, 8), "failed_creation_did_not_leak_count");
    pthread_t recovered_thread;
    CHECK(!pthread_create(&recovered_thread, NULL, immediate, (void *)2), "clone_after_limit_recovery");
    void *value = NULL;
    CHECK(!pthread_join(recovered_thread, &value) && value == (void *)2, "clone_recovery_join");
    puts("PASS glibc_nproc_thread_shared_fork_copy");
    puts("PASS glibc_nproc_real_uid_global_threads");
    puts("PASS glibc_nproc_zombie_reap_accounting");
    puts("PASS glibc_nproc_rejected_creation_recovery");
    return 0;
}
static int exec_inheritance(void)
{
    CHECK(!setresuid(UID_EXEC, UID_EXEC, UID_EXEC) && !set_limit(3, 8), "exec_setup");
    char *arguments[] = {SELF, "--inherit-exec", NULL};
    execv(SELF, arguments);
    return fail("exec_inherited_limits");
}
static int soft_lowering_exec(void)
{
    CHECK(!setresuid(UID_LOWER, UID_LOWER, UID_LOWER) && !set_limit(0, 8), "soft_lower_exec_setup");
    char *arguments[] = {SELF, "--lower-exec", NULL};
    execv(SELF, arguments);
    return fail("ordinary_soft_lowering_must_not_block_exec");
}
static int effective_root(void)
{
    CHECK(!set_limit(0, 0) && !setresuid(UID_EFFECTIVE, 0, 0) &&
          getuid() == UID_EFFECTIVE && geteuid() == 0, "real_uid_not_effective_uid_setup");
    CHECK(!fork_rejected("effective_root_still_real_uid_limited") &&
          !thread_rejected("effective_root_clone_still_limited"), "real_uid_admission_exemption_only");
    puts("PASS glibc_nproc_effective_root_not_exempt");
    return 0;
}
static int uid_transition(void)
{
    int command[2], report[2];
    CHECK(!pipe(command) && !pipe(report) && !set_limit(1, 8), "uid_transition_setup");
    pid_t holder = fork();
    if (!holder) {
        close(command[1]); close(report[0]);
        if (setresuid(UID_TRANSITION, UID_TRANSITION, UID_TRANSITION) ||
            put_byte(report[1], 'R') || get_byte(command[0], 'X')) _exit(97);
        _exit(0);
    }
    CHECK(holder > 0 && !close(command[0]) && !close(report[1]) && !get_byte(report[0], 'R'),
          "target_uid_already_has_one_task");
    CHECK(!setresuid(UID_TRANSITION, UID_TRANSITION, UID_TRANSITION) && limits_are(1, 8),
          "setuid_succeeds_over_nproc");
    CHECK(!fork_rejected("changed_uid_over_budget"), "uid_account_transfer");
    char *arguments[] = {SELF, "--transition-exec", NULL};
    errno = 0;
    CHECK(execv(SELF, arguments) == -1 && errno == EAGAIN && getuid() == UID_TRANSITION &&
          limits_are(1, 8), "deferred_exec_eagain_preserves_image");
    CHECK(!setresuid(UID_TRANSITION, UID_TRANSITION, UID_TRANSITION), "same_uid_noop");
    ERROR(execv(SELF, arguments), EAGAIN, "same_uid_noop_preserves_deferred_flag");
    CHECK(!setresuid((uid_t)-1, UID_TRANSITION, (uid_t)-1), "effective_uid_only_noop");
    ERROR(execv(SELF, arguments), EAGAIN, "effective_uid_only_preserves_deferred_flag");
    CHECK(!put_byte(command[1], 'X') && !reap(holder, "uid_holder_reap") &&
          !close(command[1]) && !close(report[0]), "uid_budget_recovered");
    execv(SELF, arguments);
    return fail("exec_after_uid_count_recovers");
}
int main(int argc, char **argv)
{
    if (set_deadline(90)) return 97;
    if (argc > 1 && !strcmp(argv[1], "--inherit-exec")) {
        CHECK(getuid() == UID_EXEC && limits_are(3, 8), "exec_limit_and_uid_preserved");
        puts("PASS glibc_nproc_exec_preserves_limits");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--lower-exec")) {
        CHECK(getuid() == UID_LOWER && limits_are(0, 8) &&
              !fork_rejected("lowered_limit_still_blocks_new_task"), "ordinary_lowering_exec_preserved");
        puts("PASS glibc_nproc_soft_lowering_exec_allowed");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--transition-exec")) {
        CHECK(getuid() == UID_TRANSITION && limits_are(1, 8) &&
              !fork_rejected("exec_existing_task_not_extra_admission"), "transition_exec_recovered");
        puts("PASS glibc_nproc_uid_transition_exec_admission");
        return 0;
    }
    CHECK(getuid() == 0 && geteuid() == 0, "initial_root");
    if (isolated(queries_and_rollback, "query_and_rollback") ||
        isolated(root_exemption, "root_exemption") || isolated(hard_raise, "hard_raise") ||
        isolated(task_accounting, "task_accounting") || isolated(exec_inheritance, "exec_inheritance") ||
        isolated(soft_lowering_exec, "soft_lowering_exec") || isolated(effective_root, "effective_root") ||
        isolated(uid_transition, "uid_transition")) return 97;
    puts("PASS glibc_posix_nproc");
    return 0;
}
