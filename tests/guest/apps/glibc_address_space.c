/* Copyright (C) 2026 NOS contributors. Real glibc address-space admission. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_BYTES 4096u
#define INITIAL_LIMIT (UINT64_C(1) << 30)
#define CHECK(condition, step) do { if (!(condition)) return fail(step); } while (0)
#define ERROR(operation, code, step) do { errno = 0; CHECK((intptr_t)(operation) == -1 && errno == (code), step); } while (0)
struct limit_pair { uint64_t soft, hard; };
_Static_assert(sizeof(struct limit_pair) == 16, "prlimit64 layout");
static const char *program_path;

static int fail(const char *step)
{
    fprintf(stderr, "FAIL glibc_address_space %s errno=%d\n", step, errno);
    return 99;
}
static void deadline(int number)
{
    static const char message[] = "FAIL glibc_address_space child_deadline_10s\n";
    (void)number;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(98);
}
static int arm_deadline(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = deadline;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGALRM, &action, NULL), "deadline_handler");
    alarm(10);
    return 0;
}
static int reap(pid_t child)
{
    int status;
    pid_t result;
    do result = waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
    if (result == child && WIFEXITED(status) && !WEXITSTATUS(status)) return 0;
    if (result == child) fprintf(stderr, "address-space child status=%d\n", status);
    return fail("child_status");
}
static int isolated(int (*test)(void))
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) _exit(arm_deadline() ? 99 : test());
    return child < 0 ? fail("isolated_fork") : reap(child);
}
static int raw_limit(const void *replacement, void *old)
{ return (int)syscall(SYS_prlimit64, 0, RLIMIT_AS, replacement, old); }
static int set_limit(uint64_t soft, uint64_t hard)
{
    struct limit_pair value = {soft, hard};
    return raw_limit(&value, NULL);
}
static int limits_are(uint64_t soft, uint64_t hard)
{
    struct rlimit narrow;
    struct rlimit64 wide, process;
    struct limit_pair raw;
    uint64_t narrow_soft = soft > (uint64_t)RLIM_INFINITY ? (uint64_t)RLIM_INFINITY : soft;
    uint64_t narrow_hard = hard > (uint64_t)RLIM_INFINITY ? (uint64_t)RLIM_INFINITY : hard;
    return !getrlimit(RLIMIT_AS, &narrow) && !getrlimit64(RLIMIT_AS, &wide) &&
        !prlimit64(getpid(), RLIMIT_AS, NULL, &process) && !raw_limit(NULL, &raw) &&
        narrow.rlim_cur == narrow_soft && narrow.rlim_max == narrow_hard &&
        wide.rlim_cur == soft && wide.rlim_max == hard &&
        process.rlim_cur == soft && process.rlim_max == hard && raw.soft == soft && raw.hard == hard;
}
static void *anonymous(size_t length, int prot)
{ return mmap(NULL, length, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); }

/* Infer the actual admission boundary without a test-only kernel counter.
 * Only raw limit calls and a one-page mmap/munmap occur during the search;
 * libc output/allocation would invalidate the inferred mapped-byte total. */
static uint64_t current_usage(void)
{
    uint64_t low = 0, high = INITIAL_LIMIT / PAGE_BYTES;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (set_limit(middle * PAGE_BYTES, INITIAL_LIMIT)) return UINT64_MAX;
        void *page = anonymous(PAGE_BYTES, PROT_NONE);
        if (page == MAP_FAILED) {
            if (errno != ENOMEM) return UINT64_MAX;
            low = middle + 1;
        } else {
            if (munmap(page, PAGE_BYTES)) return UINT64_MAX;
            high = middle;
        }
    }
    if (set_limit(INITIAL_LIMIT, INITIAL_LIMIT) || !low) return UINT64_MAX;
    return low * PAGE_BYTES - PAGE_BYTES;
}
static int query_and_privilege(void)
{
    CHECK(limits_are(INITIAL_LIMIT, INITIAL_LIMIT), "initial_query_routes");
    struct rlimit ordinary = {INITIAL_LIMIT / 2, INITIAL_LIMIT};
    CHECK(!setrlimit(RLIMIT_AS, &ordinary) && limits_are(INITIAL_LIMIT / 2, INITIAL_LIMIT),
          "native_setrlimit_shared_query_routes");
    struct rlimit64 wide = {UINT64_MAX, UINT64_MAX};
    CHECK(!setrlimit64(RLIMIT_AS, &wide) && limits_are(UINT64_MAX, UINT64_MAX),
          "infinity_is_admission_policy_not_physical_capacity");
    ERROR(set_limit(UINT64_MAX, INITIAL_LIMIT), EINVAL, "full_width_soft_hard_order");
    CHECK(limits_are(UINT64_MAX, UINT64_MAX), "invalid_order_unchanged");
    CHECK(!set_limit(INITIAL_LIMIT / 2, INITIAL_LIMIT) && !setuid(70001), "drop_privilege");
    ERROR(set_limit(INITIAL_LIMIT / 2, UINT64_MAX), EPERM, "unprivileged_hard_raise");
    CHECK(limits_are(INITIAL_LIMIT / 2, INITIAL_LIMIT) &&
          !set_limit(INITIAL_LIMIT / 4, INITIAL_LIMIT / 2) &&
          !set_limit(INITIAL_LIMIT / 2, INITIAL_LIMIT / 2), "soft_raise_within_lowered_hard");
    ERROR(set_limit(INITIAL_LIMIT / 2, INITIAL_LIMIT), EPERM, "cannot_restore_discarded_hard");
    return 0;
}
static int pointer_rollback(void)
{
    unsigned char *memory = anonymous(2 * PAGE_BYTES, PROT_READ | PROT_WRITE);
    CHECK(memory != MAP_FAILED && !mprotect(memory + PAGE_BYTES, PAGE_BYTES, PROT_NONE), "pointer_guard");
    struct limit_pair replacement = {0, INITIAL_LIMIT}, old = {123, 456};
    ERROR(raw_limit(&replacement, (void *)(uintptr_t)1), EFAULT, "bad_old_pointer");
    CHECK(limits_are(INITIAL_LIMIT, INITIAL_LIMIT), "bad_old_keeps_limit");
    ERROR(raw_limit((void *)(uintptr_t)1, &old), EFAULT, "bad_new_pointer");
    CHECK(old.soft == 123 && old.hard == 456, "bad_new_keeps_output");
    uint64_t *edge = (uint64_t *)(void *)(memory + PAGE_BYTES - sizeof(uint64_t));
    *edge = UINT64_C(0x123456789abcdef0);
    ERROR(raw_limit(&replacement, edge), EFAULT, "old_crosses_guard");
    CHECK(*edge == UINT64_C(0x123456789abcdef0) && limits_are(INITIAL_LIMIT, INITIAL_LIMIT),
          "guard_preflight_no_partial_output_or_commit");
    ERROR(raw_limit(edge, &old), EFAULT, "new_crosses_guard");
    CHECK(old.soft == 123 && old.hard == 456, "guard_new_keeps_output");
    struct limit_pair alias = {INITIAL_LIMIT / 2, INITIAL_LIMIT};
    CHECK(!raw_limit(&alias, &alias) && alias.soft == INITIAL_LIMIT && alias.hard == INITIAL_LIMIT &&
          limits_are(INITIAL_LIMIT / 2, INITIAL_LIMIT), "aliased_candidate_before_old_output");
    CHECK(!munmap(memory, 2 * PAGE_BYTES), "pointer_guard_release");
    return 0;
}
static int rounding_and_reclaim(void)
{
    uint64_t usage = current_usage();
    CHECK(usage != UINT64_MAX && usage > PAGE_BYTES, "discover_mapped_usage");
    CHECK(!set_limit(usage + PAGE_BYTES - 1, INITIAL_LIMIT), "one_byte_below_page_budget");
    ERROR(anonymous(1, PROT_NONE), ENOMEM, "mmap_length_rounds_up");
    CHECK(!set_limit(usage + PAGE_BYTES, INITIAL_LIMIT), "exact_page_budget");
    unsigned char *page = anonymous(1, PROT_NONE);
    CHECK(page != MAP_FAILED, "exact_page_reservation");
    ERROR(anonymous(1, PROT_NONE), ENOMEM, "reservation_consumes_address_space");
    CHECK(!set_limit(0, INITIAL_LIMIT) && !mprotect(page, PAGE_BYTES, PROT_READ | PROT_WRITE),
          "mprotect_existing_reservation_has_no_new_address_charge");
    page[0] = 0x51; page[PAGE_BYTES - 1] = 0x79;
    CHECK(!mprotect(page, PAGE_BYTES, PROT_NONE) &&
          !mprotect(page, PAGE_BYTES, PROT_READ | PROT_WRITE) && page[0] == 0x51 &&
          page[PAGE_BYTES - 1] == 0x79, "lowered_limit_preserves_existing_backing");
    ERROR(anonymous(PAGE_BYTES, PROT_READ | PROT_WRITE), ENOMEM, "zero_soft_denies_new_mapping");
    CHECK(!munmap(page, PAGE_BYTES) && !set_limit(usage + PAGE_BYTES, INITIAL_LIMIT), "unmap_reclaims_charge");
    page = anonymous(PAGE_BYTES, PROT_READ | PROT_WRITE);
    CHECK(page != MAP_FAILED && !munmap(page, PAGE_BYTES) && !set_limit(INITIAL_LIMIT, INITIAL_LIMIT),
          "reclaimed_page_is_readmitted");
    return 0;
}
static int large_reservation(void)
{
    const size_t bytes = 2 * 1024 * 1024;
    uint64_t usage = current_usage();
    CHECK(usage != UINT64_MAX && !set_limit(usage + bytes, INITIAL_LIMIT), "large_reservation_budget");
    unsigned char *area = anonymous(bytes, PROT_NONE);
    CHECK(area != MAP_FAILED, "large_unbacked_reservation");
    ERROR(anonymous(PAGE_BYTES, PROT_NONE), ENOMEM, "entire_unbacked_reservation_is_charged");
    CHECK(!mprotect(area + PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE), "reservation_split_no_double_charge");
    area[PAGE_BYTES] = 0x43;
    CHECK(!munmap(area + PAGE_BYTES, PAGE_BYTES), "partial_unmap_reclaims_one_page");
    void *page = anonymous(PAGE_BYTES, PROT_NONE);
    CHECK(page != MAP_FAILED, "one_page_reclaimed_from_split");
    ERROR(anonymous(PAGE_BYTES, PROT_NONE), ENOMEM, "split_metadata_does_not_lose_charge");
    CHECK(!munmap(page, PAGE_BYTES) && !munmap(area, bytes) &&
          !set_limit(INITIAL_LIMIT, INITIAL_LIMIT), "large_reservation_cleanup_with_hole");
    return 0;
}
static int fixed_transactions(void)
{
    unsigned char *area = anonymous(4 * PAGE_BYTES, PROT_READ | PROT_WRITE);
    CHECK(area != MAP_FAILED && !munmap(area + 3 * PAGE_BYTES, PAGE_BYTES), "fixed_adjacent_hole");
    area[0] = 0x17; area[PAGE_BYTES] = 0x29; area[2 * PAGE_BYTES] = 0x3b;
    uint64_t usage = current_usage();
    CHECK(usage != UINT64_MAX && !set_limit(usage, INITIAL_LIMIT), "fixed_exact_current_usage");
    void *middle = mmap(area + PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(middle == area + PAGE_BYTES && !area[PAGE_BYTES] && area[0] == 0x17 &&
          area[2 * PAGE_BYTES] == 0x3b, "fixed_replacement_uses_net_charge");
    ERROR(mmap(area + 2 * PAGE_BYTES, 2 * PAGE_BYTES, PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), ENOMEM, "fixed_expansion_denied");
    CHECK(area[0] == 0x17 && area[2 * PAGE_BYTES] == 0x3b, "failed_fixed_keeps_original_contents");
    ERROR(mmap(area, PAGE_BYTES, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), EEXIST,
          "noreplace_collision_preserves_error");
    CHECK(!set_limit(0, INITIAL_LIMIT), "lower_limit_below_existing_maps");
    CHECK(mmap(area + PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == area + PAGE_BYTES,
          "same_size_fixed_allowed_above_lowered_limit");
    CHECK(!munmap(area, 3 * PAGE_BYTES) && !set_limit(INITIAL_LIMIT, INITIAL_LIMIT), "fixed_cleanup");
    return 0;
}
static int shared_mapping(void)
{
    uint64_t usage = current_usage();
    CHECK(usage != UINT64_MAX && !set_limit(usage + PAGE_BYTES, INITIAL_LIMIT), "shared_budget");
    unsigned char *page = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED, "shared_mapping_admitted");
    page[0] = 0x65;
    ERROR(mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0),
          ENOMEM, "shared_mapping_counts_virtual_bytes");
    CHECK(page[0] == 0x65 && !munmap(page, PAGE_BYTES), "shared_denial_keeps_mapping");
    page = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED && !munmap(page, PAGE_BYTES) && !set_limit(INITIAL_LIMIT, INITIAL_LIMIT),
          "shared_failed_admission_reclaims_object");
    return 0;
}
static int heap_admission(void)
{
    uintptr_t original = (uintptr_t)syscall(SYS_brk, 0);
    uintptr_t rounded = (original + PAGE_BYTES - 1) & ~(uintptr_t)(PAGE_BYTES - 1);
    uintptr_t target = rounded + PAGE_BYTES;
    uint64_t usage = current_usage();
    CHECK(usage != UINT64_MAX && !set_limit(usage, INITIAL_LIMIT), "brk_budget");
    CHECK((uintptr_t)syscall(SYS_brk, target) == original &&
          (uintptr_t)syscall(SYS_brk, 0) == original, "denied_brk_returns_old_break");
    CHECK(!set_limit(usage + PAGE_BYTES, INITIAL_LIMIT) &&
          (uintptr_t)syscall(SYS_brk, target) == target, "brk_exact_page_growth");
    *(volatile unsigned char *)(target - 1) = 0x71;
    ERROR(anonymous(PAGE_BYTES, PROT_NONE), ENOMEM, "brk_growth_consumes_mapping_budget");
    CHECK((uintptr_t)syscall(SYS_brk, original) == original, "brk_shrink");
    void *page = anonymous(PAGE_BYTES, PROT_NONE);
    CHECK(page != MAP_FAILED && !munmap(page, PAGE_BYTES) && !set_limit(INITIAL_LIMIT, INITIAL_LIMIT),
          "brk_shrink_reclaims_charge");
    return 0;
}

static pthread_barrier_t thread_barrier;
static int worker_result;
static uint64_t thread_usage;
static int barrier(void)
{
    int result = pthread_barrier_wait(&thread_barrier);
    return result && result != PTHREAD_BARRIER_SERIAL_THREAD;
}
static void *limit_worker(void *argument)
{
    (void)argument;
    if (barrier()) worker_result = 1;
    if (!limits_are(thread_usage, INITIAL_LIMIT)) worker_result = 2;
    errno = 0;
    if (anonymous(PAGE_BYTES, PROT_NONE) != MAP_FAILED || errno != ENOMEM) worker_result = 3;
    if (set_limit(0, INITIAL_LIMIT)) worker_result = 4;
    if (barrier()) worker_result = 5;
    if (barrier()) worker_result = 6;
    return NULL;
}
static int thread_shared_fork_copy(void)
{
    CHECK(!pthread_barrier_init(&thread_barrier, NULL, 2), "thread_barrier_setup");
    pthread_t worker;
    CHECK(!pthread_create(&worker, NULL, limit_worker, NULL), "thread_create_before_lowering");
    thread_usage = current_usage();
    CHECK(thread_usage != UINT64_MAX && !set_limit(thread_usage, INITIAL_LIMIT), "thread_shared_usage_budget");
    CHECK(!barrier() && !barrier() && limits_are(0, INITIAL_LIMIT), "thread_updates_shared_admission");
    CHECK(!set_limit(INITIAL_LIMIT, INITIAL_LIMIT) && !barrier() && !pthread_join(worker, NULL) &&
          !worker_result && !pthread_barrier_destroy(&thread_barrier), "thread_join_after_restore");
    CHECK(!set_limit(0, INITIAL_LIMIT), "fork_below_current_usage_setup");
    pid_t child = fork();
    if (!child) {
        if (!limits_are(0, INITIAL_LIMIT)) _exit(fail("fork_inherits_lowered_limit"));
        errno = 0;
        if (anonymous(PAGE_BYTES, PROT_NONE) != MAP_FAILED || errno != ENOMEM)
            _exit(fail("fork_inherits_actual_admission"));
        _exit(set_limit(INITIAL_LIMIT / 2, INITIAL_LIMIT) ? fail("child_independent_limit") : 0);
    }
    CHECK(child > 0 && !reap(child) && limits_are(0, INITIAL_LIMIT),
          "fork_duplicate_existing_space_does_not_grow_parent_or_share_limits");
    CHECK(!set_limit(INITIAL_LIMIT, INITIAL_LIMIT), "fork_parent_restore");
    return 0;
}
static int exec_stage(int argc, char **argv)
{
    CHECK(argc == 3 && getpid() == (pid_t)strtol(argv[2], NULL, 10), "exec_pid");
    CHECK(limits_are(64u * 1024u * 1024u, 128u * 1024u * 1024u), "exec_preserves_limit_object");
    CHECK(!set_limit(0, 128u * 1024u * 1024u), "exec_limit_remains_mutable");
    ERROR(anonymous(PAGE_BYTES, PROT_NONE), ENOMEM, "post_exec_admission_enforced");
    return 0;
}
static void *exec_worker(void *argument)
{
    extern char **environ;
    char **arguments = argument;
    execve(arguments[0], arguments, environ);
    _exit(fail("nonleader_exec"));
}
static int exec_case(int nonleader)
{
    CHECK(!set_limit(0, INITIAL_LIMIT), "exec_zero_limit");
    extern char **environ;
    char *arguments[] = {(char *)program_path, "exec-as", NULL, NULL};
    ERROR(execve(program_path, arguments, environ), ENOMEM, "exec_new_image_over_limit");
    CHECK(limits_are(0, INITIAL_LIMIT), "failed_exec_keeps_image_and_limits");
    CHECK(!set_limit(64u * 1024u * 1024u, 128u * 1024u * 1024u), "exec_finite_budget");
    char pid[24];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    arguments[2] = pid;
    if (nonleader) {
        pthread_t worker;
        CHECK(!pthread_create(&worker, NULL, exec_worker, arguments), "exec_worker_create");
        (void)pthread_join(worker, NULL);
        return fail("nonleader_exec_returned");
    }
    execve(program_path, arguments, environ);
    return fail("leader_exec");
}
static int leader_exec(void) { return exec_case(0); }
static int nonleader_exec(void) { return exec_case(1); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exec-as")) return arm_deadline() ? 99 : exec_stage(argc, argv);
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "qemu")), "arguments");
    CHECK(sysconf(_SC_PAGESIZE) == PAGE_BYTES && limits_are(INITIAL_LIMIT, INITIAL_LIMIT), "initial_profile");
    program_path = argv[0];
    CHECK(!isolated(query_and_privilege), "query_and_privilege_child");
    puts("PASS glibc_as_query_privilege");
    CHECK(!isolated(pointer_rollback), "pointer_rollback_child");
    puts("PASS glibc_as_pointer_rollback");
    CHECK(!isolated(rounding_and_reclaim), "rounding_child");
    puts("PASS glibc_as_rounding_mprotect_reclaim");
    CHECK(!isolated(large_reservation), "reservation_child");
    puts("PASS glibc_as_prot_none_split");
    CHECK(!isolated(fixed_transactions), "fixed_child");
    puts("PASS glibc_as_fixed_net_transaction");
    CHECK(!isolated(shared_mapping), "shared_child");
    puts("PASS glibc_as_shared_mapping_charge");
    CHECK(!isolated(heap_admission), "heap_child");
    puts("PASS glibc_as_brk_admission");
    CHECK(!isolated(thread_shared_fork_copy), "thread_fork_child");
    puts("PASS glibc_as_thread_shared_fork_copy");
    CHECK(!isolated(leader_exec), "leader_exec_child");
    puts("PASS glibc_as_exec_admission_preserve");
    CHECK(!isolated(nonleader_exec), "nonleader_exec_child");
    puts("PASS glibc_as_nonleader_exec");
    CHECK(limits_are(INITIAL_LIMIT, INITIAL_LIMIT), "parent_limits_unchanged");
    puts("PASS glibc_posix_address_space");
    return 0;
}
