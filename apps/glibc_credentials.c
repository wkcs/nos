/* Copyright (C) 2026 NOS contributors. Real glibc process identity checks. */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* This is the implemented NOS bound, independently checked through the real
 * unmodified libc's proc-backed sysconf query before exercising the limit. */
#define GROUP_LIMIT 32
#define TEST_UID ((uid_t)70001)
#define TEST_GID ((gid_t)80001)

_Static_assert(sizeof(uid_t) == 4, "Linux UID32 ABI");
_Static_assert(sizeof(gid_t) == 4, "Linux GID32 ABI");

struct identity {
    uid_t uid[3];
    gid_t gid[3];
    int count;
    gid_t groups[GROUP_LIMIT];
};

/* Diagnostic breadcrumbs only: success still depends on the exact identity
 * assertions and barriers below. Every value fits in two decimal digits. */
static sig_atomic_t xid_stage = -1;
static sig_atomic_t xid_phase = -1;
static sig_atomic_t xid_worker_phase[2];
_Static_assert(__atomic_always_lock_free(sizeof(sig_atomic_t), 0), "signal-safe breadcrumbs");
#define XID_SET(variable, value) __atomic_store_n(&(variable), (sig_atomic_t)(value), __ATOMIC_RELAXED)
#define XID_READ(variable) __atomic_load_n(&(variable), __ATOMIC_RELAXED)

static int die(const char *step)
{
    fprintf(stderr, "FAIL glibc_credentials %s errno=%d\n", step, errno);
    return 97;
}

static void deadline(int signal)
{
    static const char message[] = "FAIL glibc_credentials child_deadline_10s\n";
    (void)signal;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    sig_atomic_t stage = XID_READ(xid_stage);
    if (stage >= 0) {
        char details[] = "credentials setxid deadline stage=00 phase=00 worker0=00 worker1=00\n";
        const unsigned offsets[] = {
            sizeof("credentials setxid deadline stage=") - 1,
            sizeof("credentials setxid deadline stage=00 phase=") - 1,
            sizeof("credentials setxid deadline stage=00 phase=00 worker0=") - 1,
            sizeof("credentials setxid deadline stage=00 phase=00 worker0=00 worker1=") - 1
        };
        sig_atomic_t values[] = {stage, XID_READ(xid_phase),
            XID_READ(xid_worker_phase[0]), XID_READ(xid_worker_phase[1])};
        for (unsigned i = 0; i < 4; i++) {
            details[offsets[i]] = values[i] < 0 ? '?' : (char)('0' + values[i] / 10);
            details[offsets[i] + 1] = values[i] < 0 ? '?' : (char)('0' + values[i] % 10);
        }
        (void)write(STDERR_FILENO, details, sizeof(details) - 1);
    }
    _exit(98);
}

static int arm_deadline(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = deadline;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return die("deadline_handler");
    alarm(10);
    return 0;
}

static int wait_ok(pid_t child, const char *step)
{
    int status;
    pid_t result;
    do result = waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
    if (result == child && WIFEXITED(status) && !WEXITSTATUS(status)) return 0;
    if (result == child)
        fprintf(stderr, "credentials child %s status=%d\n", step, status);
    return die(step);
}

static int isolated(int (*test)(void), const char *step)
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        if (arm_deadline()) _exit(97);
        _exit(test());
    }
    return child < 0 ? die("isolated_fork") : wait_ok(child, step);
}

/* Supplementary group order is not part of the POSIX API contract. Duplicate
 * membership is deliberately checked as a multiset, not silently discarded. */
static int same_groups(const gid_t *left, const gid_t *right, int count)
{
    unsigned char used[GROUP_LIMIT] = {0};
    for (int i = 0; i < count; i++) {
        int j;
        for (j = 0; j < count; j++)
            if (!used[j] && left[i] == right[j]) { used[j] = 1; break; }
        if (j == count) return 0;
    }
    return 1;
}

static int snapshot(struct identity *value)
{
    memset(value, 0, sizeof(*value));
    if (getresuid(&value->uid[0], &value->uid[1], &value->uid[2]) ||
        getresgid(&value->gid[0], &value->gid[1], &value->gid[2])) return -1;
    value->count = getgroups(GROUP_LIMIT, value->groups);
    if (value->count < 0 || value->count > GROUP_LIMIT) return -1;
    return getuid() == value->uid[0] && geteuid() == value->uid[1] &&
        getgid() == value->gid[0] && getegid() == value->gid[1] ? 0 : -1;
}

static int same_identity(const struct identity *left, const struct identity *right)
{
    return !memcmp(left->uid, right->uid, sizeof(left->uid)) &&
        !memcmp(left->gid, right->gid, sizeof(left->gid)) &&
        left->count == right->count && same_groups(left->groups, right->groups, left->count);
}

static int current_ids(uid_t real, uid_t effective, uid_t saved,
                       gid_t greal, gid_t geffective, gid_t gsaved)
{
    uid_t u[3];
    gid_t g[3];
    return !getresuid(&u[0], &u[1], &u[2]) && !getresgid(&g[0], &g[1], &g[2]) &&
        u[0] == real && u[1] == effective && u[2] == saved &&
        g[0] == greal && g[1] == geffective && g[2] == gsaved &&
        getuid() == real && geteuid() == effective && getgid() == greal && getegid() == geffective;
}

static int initial_auxv(void)
{
    struct identity current;
    if (snapshot(&current) || !current_ids(0, 0, 0, 0, 0, 0) ||
        getauxval(AT_UID) != getuid() || getauxval(AT_EUID) != geteuid() ||
        getauxval(AT_GID) != getgid() || getauxval(AT_EGID) != getegid() || getauxval(AT_SECURE))
        return die("initial_root_getters_and_auxv");
    puts("PASS glibc_credentials_initial_auxv");
    return 0;
}

static int id_width_child(void)
{
    if (setresgid(TEST_GID, TEST_GID + 1, TEST_GID + 2) ||
        setresuid(TEST_UID, TEST_UID + 1, TEST_UID + 2) ||
        !current_ids(TEST_UID, TEST_UID + 1, TEST_UID + 2, TEST_GID, TEST_GID + 1, TEST_GID + 2))
        return die("uid_gid_above_65535");
    if (setresuid((uid_t)-1, (uid_t)-1, (uid_t)-1) ||
        setresgid((gid_t)-1, (gid_t)-1, (gid_t)-1) ||
        !current_ids(TEST_UID, TEST_UID + 1, TEST_UID + 2, TEST_GID, TEST_GID + 1, TEST_GID + 2))
        return die("all_unchanged_sentinels");
    return 0;
}

static int largest_id_child(void)
{
    uid_t uid = (uid_t)-2;
    gid_t gid = (gid_t)-2;
    if (setgid(gid) || setuid(uid) || !current_ids(uid, uid, uid, gid, gid, gid))
        return die("largest_valid_uid_gid_are_unsigned");
    return 0;
}

static int sign_bit_id_child(void)
{
    uid_t uid = UINT32_C(0x80000000);
    gid_t gid = UINT32_C(0x80000000);
    if (setgid(gid) || setuid(uid) || !current_ids(uid, uid, uid, gid, gid, gid))
        return die("sign_bit_uid_gid_are_actual_values");
    return 0;
}

static int saved_uid_child(void)
{
    if (setresuid(TEST_UID, 0, 0) || seteuid(TEST_UID) ||
        !current_ids(TEST_UID, TEST_UID, 0, 0, 0, 0) || seteuid(0) ||
        !current_ids(TEST_UID, 0, 0, 0, 0, 0)) return die("saved_root_uid_drop_regain");
    /* EUID == old RUID leaves the saved ID untouched when RUID is omitted. */
    if (setreuid((uid_t)-1, TEST_UID) || !current_ids(TEST_UID, TEST_UID, 0, 0, 0, 0) ||
        seteuid(0)) return die("setreuid_old_real_preserves_saved");
    if (setresuid(TEST_UID, 0, TEST_UID + 2) || setreuid(TEST_UID, (uid_t)-1) ||
        !current_ids(TEST_UID, 0, 0, 0, 0, 0)) return die("setreuid_explicit_real_updates_saved");
    /* A different requested EUID replaces the saved ID with the new EUID. */
    if (setreuid((uid_t)-1, TEST_UID + 1) ||
        !current_ids(TEST_UID, TEST_UID + 1, TEST_UID + 1, 0, 0, 0))
        return die("setreuid_new_effective_updates_saved");
    errno = 0;
    if (seteuid(0) != -1 || errno != EPERM || seteuid(TEST_UID) || setuid(TEST_UID + 1) ||
        !current_ids(TEST_UID, TEST_UID + 1, TEST_UID + 1, 0, 0, 0))
        return die("unprivileged_uid_selects_real_or_saved");
    return 0;
}

static int saved_gid_child(void)
{
    if (setresgid(TEST_GID, 0, 0) || setegid(TEST_GID) ||
        !current_ids(0, 0, 0, TEST_GID, TEST_GID, 0) || setegid(0) ||
        setregid((gid_t)-1, TEST_GID) || !current_ids(0, 0, 0, TEST_GID, TEST_GID, 0) ||
        setegid(0) || setresgid(TEST_GID, 0, TEST_GID + 2) ||
        setregid(TEST_GID, (gid_t)-1) || !current_ids(0, 0, 0, TEST_GID, 0, 0) ||
        setregid((gid_t)-1, TEST_GID + 1) ||
        !current_ids(0, 0, 0, TEST_GID, TEST_GID + 1, TEST_GID + 1) || setuid(TEST_UID))
        return die("saved_gid_and_setregid_rules");
    errno = 0;
    if (setegid(0) != -1 || errno != EPERM || setegid(TEST_GID) || setgid(TEST_GID + 1) ||
        !current_ids(TEST_UID, TEST_UID, TEST_UID, TEST_GID, TEST_GID + 1, TEST_GID + 1))
        return die("unprivileged_gid_selects_real_or_saved");
    return 0;
}

static int irreversible_child(void)
{
    struct identity before, after;
    if (setgid(TEST_GID) || setuid(TEST_UID) || snapshot(&before) ||
        !current_ids(TEST_UID, TEST_UID, TEST_UID, TEST_GID, TEST_GID, TEST_GID))
        return die("permanent_uid_gid_drop");
    errno = 0;
    if (setuid(0) != -1 || errno != EPERM) return die("permanent_drop_setuid_root_denied");
    errno = 0;
    if (setgid(0) != -1 || errno != EPERM) return die("permanent_drop_setgid_root_denied");
    errno = 0;
    if (setresuid(0, (uid_t)-1, (uid_t)-1) != -1 || errno != EPERM)
        return die("permanent_drop_setresuid_denied");
    errno = 0;
    if (setresgid((gid_t)-1, 0, (gid_t)-1) != -1 || errno != EPERM)
        return die("permanent_drop_setresgid_denied");
    errno = 0;
    if (setresuid(TEST_UID, TEST_UID, 0) != -1 || errno != EPERM)
        return die("setresuid_late_saved_id_denial_atomic");
    errno = 0;
    if (setresgid(TEST_GID, TEST_GID, 0) != -1 || errno != EPERM)
        return die("setresgid_late_saved_id_denial_atomic");
    if (snapshot(&after) || !same_identity(&before, &after)) return die("denied_ids_unchanged");
    if (setuid(TEST_UID) || setgid(TEST_GID)) return die("same_identity_setter_is_valid");
    return 0;
}

static int groups_child(void)
{
    gid_t wanted[GROUP_LIMIT + 1], actual[GROUP_LIMIT];
    for (int i = 0; i <= GROUP_LIMIT; i++) wanted[i] = TEST_GID + 100 + (gid_t)i;
    wanted[1] = wanted[0];
    if (sysconf(_SC_NGROUPS_MAX) != GROUP_LIMIT || setgroups(GROUP_LIMIT, wanted) ||
        getgroups(0, NULL) != GROUP_LIMIT || getgroups(GROUP_LIMIT, actual) != GROUP_LIMIT ||
        !same_groups(wanted, actual, GROUP_LIMIT)) return die("actual_ngroups_exact_limit_and_duplicates");
    volatile uintptr_t bad = 1;
    if (getgroups(0, (gid_t *)bad) != GROUP_LIMIT) return die("groups_zero_size_does_not_dereference");
    volatile int negative_count = -1;
    errno = 0;
    if (getgroups(negative_count, actual) != -1 || errno != EINVAL) return die("groups_negative_size");
    errno = 0;
    if (getgroups(GROUP_LIMIT - 1, actual) != -1 || errno != EINVAL) return die("groups_small_output");
    errno = 0;
    if (setgroups(GROUP_LIMIT + 1, wanted) != -1 || errno != EINVAL)
        return die("groups_over_limit_einval");
    if (getgroups(GROUP_LIMIT, actual) != GROUP_LIMIT || !same_groups(wanted, actual, GROUP_LIMIT))
        return die("groups_over_limit_unchanged");
    errno = 0;
    if (setgroups(GROUP_LIMIT, (const gid_t *)bad) != -1 || errno != EFAULT)
        return die("groups_bad_input");
    if (getgroups(GROUP_LIMIT, actual) != GROUP_LIMIT || !same_groups(wanted, actual, GROUP_LIMIT))
        return die("groups_bad_input_unchanged");
    wanted[GROUP_LIMIT - 1] = (gid_t)-1;
    errno = 0;
    if (setgroups(GROUP_LIMIT, wanted) != -1 || errno != EINVAL)
        return die("groups_invalid_gid");
    wanted[GROUP_LIMIT - 1] = TEST_GID + 100 + GROUP_LIMIT - 1;
    if (getgroups(GROUP_LIMIT, actual) != GROUP_LIMIT || !same_groups(wanted, actual, GROUP_LIMIT))
        return die("groups_invalid_gid_unchanged");
    if (setuid(TEST_UID)) return die("groups_drop_uid");
    errno = 0;
    if (setgroups(GROUP_LIMIT, wanted) != -1 || errno != EPERM)
        return die("unprivileged_equal_groups_denied");
    errno = 0;
    if (setgroups(0, NULL) != -1 || errno != EPERM)
        return die("unprivileged_empty_groups_denied");
    if (getgroups(GROUP_LIMIT, actual) != GROUP_LIMIT || !same_groups(wanted, actual, GROUP_LIMIT))
        return die("unprivileged_groups_unchanged");
    return 0;
}

static int empty_groups_child(void)
{
    gid_t supplementary = TEST_GID + 100;
    volatile uintptr_t invalid = 1;
    if (setgid(TEST_GID) || setgroups(1, &supplementary) || getgroups(0, NULL) != 1 ||
        setgroups(0, (const gid_t *)invalid) || getgroups(0, NULL) != 0 ||
        getgid() != TEST_GID || getegid() != TEST_GID)
        return die("empty_groups_does_not_insert_primary_gid");
    return 0;
}

static int fork_child(void)
{
    gid_t membership[] = {TEST_GID + 5, TEST_GID + 6};
    if (setgroups(2, membership) || setresgid(TEST_GID, TEST_GID + 1, TEST_GID + 2) ||
        setresuid(TEST_UID, 0, 0)) return die("fork_parent_identity_setup");
    struct identity original, after;
    if (snapshot(&original)) return die("fork_parent_snapshot");
    pid_t child = fork();
    if (!child) {
        struct identity inherited;
        if (snapshot(&inherited) || !same_identity(&original, &inherited)) _exit(die("fork_copies_identity"));
        gid_t changed = TEST_GID + 10;
        if (setgroups(1, &changed) || setgid(TEST_GID + 10) || setuid(TEST_UID + 10))
            _exit(die("fork_child_mutation"));
        _exit(current_ids(TEST_UID + 10, TEST_UID + 10, TEST_UID + 10,
                          TEST_GID + 10, TEST_GID + 10, TEST_GID + 10) ? 0 : die("fork_child_new_ids"));
    }
    if (child < 0 || wait_ok(child, "fork_child_status") || snapshot(&after) ||
        !same_identity(&original, &after)) return die("fork_child_does_not_mutate_parent");
    return 0;
}

static int fault_child(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page < 256) return die("fault_page_size");
    unsigned char *memory = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED || mprotect(memory + page, (size_t)page, PROT_NONE))
        return die("fault_mapping");
    gid_t wanted[] = {TEST_GID + 4, TEST_GID + 5};
    struct identity before, after;
    if (setgroups(2, wanted) || snapshot(&before)) return die("fault_identity_snapshot");
    volatile uintptr_t invalid = 1;
    for (unsigned which = 0; which < 3; which++) {
        uid_t u[3] = {123, 456, 789};
        uid_t *p[3] = {&u[0], &u[1], &u[2]};
        p[which] = (uid_t *)invalid;
        errno = 0;
        if (getresuid(p[0], p[1], p[2]) != -1 || errno != EFAULT ||
            u[0] != 123 || u[1] != 456 || u[2] != 789)
            return die("getresuid_all_outputs_preflight");
        gid_t g[3] = {123, 456, 789};
        gid_t *q[3] = {&g[0], &g[1], &g[2]};
        q[which] = (gid_t *)invalid;
        errno = 0;
        if (getresgid(q[0], q[1], q[2]) != -1 || errno != EFAULT ||
            g[0] != 123 || g[1] != 456 || g[2] != 789)
            return die("getresgid_all_outputs_preflight");
    }
    memset(memory + page - 4, 0x5a, 4);
    uid_t first = 123, third = 789;
    errno = 0;
    if (getresuid(&first, (uid_t *)(memory + page - 2), &third) != -1 || errno != EFAULT ||
        first != 123 || third != 789 || memory[page - 2] != 0x5a || memory[page - 1] != 0x5a)
        return die("getresuid_guard_crossing_atomic");
    memset(memory, 0x3c, sizeof(wanted));
    errno = 0;
    if (getgroups(2, (gid_t *)(memory + page - sizeof(gid_t))) != -1 || errno != EFAULT ||
        memory[page - 4] != 0x5a || memory[page - 3] != 0x5a ||
        memory[page - 2] != 0x5a || memory[page - 1] != 0x5a)
        return die("getgroups_guard_crossing_atomic");
    memcpy(memory, wanted, sizeof(wanted));
    if (mprotect(memory, (size_t)page, PROT_READ) || setgroups(2, (const gid_t *)memory))
        return die("groups_readonly_input_accepted");
    errno = 0;
    if (getgroups(2, (gid_t *)memory) != -1 || errno != EFAULT || memcmp(memory, wanted, sizeof(wanted)))
        return die("groups_readonly_output_rejected");
    first = 123; third = 789;
    errno = 0;
    if (getresuid(&first, (uid_t *)memory, &third) != -1 || errno != EFAULT || first != 123 || third != 789)
        return die("getresuid_readonly_output_atomic");
    errno = 0;
    if (setgroups(2, (const gid_t *)(memory + page - sizeof(gid_t))) != -1 || errno != EFAULT)
        return die("setgroups_guard_input_atomic");
    if (snapshot(&after) || !same_identity(&before, &after)) return die("faults_keep_complete_identity");
    errno = 0;
    if (setuid((uid_t)-1) != -1 || errno != EINVAL) return die("setuid_invalid_actual_id");
    errno = 0;
    if (setgid((gid_t)-1) != -1 || errno != EINVAL) return die("setgid_invalid_actual_id");
    if (snapshot(&after) || !same_identity(&before, &after)) return die("invalid_actual_id_rollback");
    if (mprotect(memory, (size_t)page, PROT_READ | PROT_WRITE)) return die("fault_restore_mapping");
    uid_t expected[3] = {0, 0, 0}, observed[3];
    if (getresuid((uid_t *)(memory + 1), (uid_t *)(memory + 9), (uid_t *)(memory + 17)))
        return die("getresuid_unaligned_outputs");
    for (unsigned i = 0; i < 3; i++) memcpy(&observed[i], memory + 1 + i * 8, sizeof(uid_t));
    if (memcmp(expected, observed, sizeof(expected)) || munmap(memory, (size_t)page * 2))
        return die("getresuid_unaligned_values");
    return 0;
}

static int unsupported_child(void)
{
#if defined(__arm__)
    const long uid_call = SYS_setfsuid32, gid_call = SYS_setfsgid32;
#else
    const long uid_call = SYS_setfsuid, gid_call = SYS_setfsgid;
#endif
    struct identity before, after;
    if (snapshot(&before)) return die("unsupported_snapshot");
    errno = 0;
    if (syscall(uid_call, (uid_t)-1) != -1 || errno != ENOSYS) return die("setfsuid_explicit_enosys");
    errno = 0;
    if (syscall(gid_call, (gid_t)-1) != -1 || errno != ENOSYS) return die("setfsgid_explicit_enosys");
    if (snapshot(&after) || !same_identity(&before, &after)) return die("unsupported_fsids_keep_identity");
#if defined(__arm__)
    const long old16[] = {24, 49, 47, 50, 80, 81, 23, 46, 70, 71, 164, 165, 170, 171, 138, 139};
    for (unsigned i = 0; i < sizeof(old16) / sizeof(old16[0]); i++) {
        errno = 0;
        if (syscall(old16[i], 0, 0, 0) != -1 || errno != ENOSYS) return die("arm_old16_explicit_enosys");
    }
#endif
    return 0;
}

static const gid_t exec_groups[] = {TEST_GID + 20, TEST_GID + 21};

static int prepare_exec(uid_t effective, uid_t saved)
{
    return setgroups(2, exec_groups) || setresgid(TEST_GID, TEST_GID + 1, TEST_GID + 2) ||
        setresuid(TEST_UID, effective, saved) ? die("exec_identity_setup") : 0;
}

static int prepared_identity(uid_t effective, uid_t saved)
{
    gid_t groups[2];
    return current_ids(TEST_UID, effective, saved, TEST_GID, TEST_GID + 1, TEST_GID + 2) &&
        getgroups(2, groups) == 2 && same_groups(groups, exec_groups, 2);
}

static int executed_identity(uid_t effective)
{
    gid_t groups[2];
    /* Even an ordinary, non-set-ID executable replaces saved IDs with the
     * effective IDs. This differs from fork and a leader's pthread_exit. */
    return current_ids(TEST_UID, effective, effective, TEST_GID, TEST_GID + 1, TEST_GID + 1) &&
        getgroups(2, groups) == 2 && same_groups(groups, exec_groups, 2);
}

static int exec_stage(int argc, char **argv)
{
    if (argc != 4) return die("fresh_exec_arguments");
    uid_t effective = (uid_t)strtoul(argv[3], NULL, 10);
    if ((effective != 0 && effective != TEST_UID + 1) ||
        getpid() != (pid_t)strtol(argv[2], NULL, 10) || !executed_identity(effective) ||
        getauxval(AT_UID) != TEST_UID || getauxval(AT_EUID) != effective ||
        getauxval(AT_GID) != TEST_GID || getauxval(AT_EGID) != TEST_GID + 1 ||
        getauxval(AT_SECURE) != 1) return die("fresh_exec_actual_ids_groups_auxv_secure");
    /* The non-root case deliberately had saved UID 0 before exec: retaining
     * it would permit regaining root. Root must first drop its effective UID
     * before testing denial, while preserving exec's new saved UID. */
    uid_t old_saved = effective ? 0 : TEST_UID + 2;
    if (setegid(TEST_GID) || seteuid(TEST_UID)) return die("exec_drop_effective_for_saved_id_checks");
    struct identity before, after;
    if (snapshot(&before) || !current_ids(TEST_UID, TEST_UID, effective,
                                         TEST_GID, TEST_GID, TEST_GID + 1))
        return die("exec_new_saved_ids_survive_effective_drop");
    errno = 0;
    if (seteuid(old_saved) != -1 || errno != EPERM || snapshot(&after) ||
        !same_identity(&before, &after)) return die("exec_cannot_restore_discarded_saved_uid");
    errno = 0;
    if (setegid(TEST_GID + 2) != -1 || errno != EPERM || snapshot(&after) ||
        !same_identity(&before, &after)) return die("exec_cannot_restore_discarded_saved_gid");
    return 0;
}

static void *thread_exec(void *argument)
{
    char **arguments = argument;
    extern char **environ;
    execve(arguments[0], arguments, environ);
    _exit(die("nonleader_execve"));
}

static int exec_child(const char *program, int nonleader, uid_t effective)
{
    uid_t saved = effective ? 0 : TEST_UID + 2;
    if (prepare_exec(effective, saved) || !prepared_identity(effective, saved))
        return die("exec_prepared_identity");
    struct identity before, after;
    if (snapshot(&before)) return die("exec_original_snapshot");
    extern char **environ;
    char *missing[] = {"/credentials-no-such-executable", NULL};
    errno = 0;
    if (execve(missing[0], missing, environ) != -1 || errno != ENOENT ||
        snapshot(&after) || !same_identity(&before, &after)) return die("failed_exec_identity_rollback");
    volatile uintptr_t bad = 1;
    errno = 0;
    if (execve(program, (char *const *)bad, environ) != -1 || errno != EFAULT ||
        snapshot(&after) || !same_identity(&before, &after)) return die("failed_exec_fault_identity_rollback");
    char pid[16], effective_text[16];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    snprintf(effective_text, sizeof(effective_text), "%lu", (unsigned long)effective);
    char *arguments[] = {(char *)program, "exec-identity", pid, effective_text, NULL};
    if (nonleader) {
        pthread_t worker;
        int result = pthread_create(&worker, NULL, thread_exec, arguments);
        if (result) { errno = result; return die("nonleader_create"); }
        result = pthread_join(worker, NULL);
        errno = result;
        return die("nonleader_exec_returned");
    }
    execve(program, arguments, environ);
    return die("ordinary_execve");
}

static int exec_case(const char *program, int nonleader, uid_t effective)
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        if (arm_deadline()) _exit(97);
        _exit(exec_child(program, nonleader, effective));
    }
    return child < 0 ? die("exec_case_fork") : wait_ok(child, nonleader ? "nonleader_exec_status" : "exec_status");
}

static void child_notification(int number)
{ (void)number; }
static int leader_exit_signal_mode;
static sig_atomic_t exit_peer_ready;

static void *exit_group_peer(void *argument)
{
    (void)argument;
    __atomic_store_n(&exit_peer_ready, 1, __ATOMIC_RELEASE);
    /* Remain in user space so SMP termination can cancel a running peer. */
    for (;;) __asm__ volatile("" ::: "memory");
    return NULL;
}

static void *after_leader_exit(void *argument)
{
    pthread_t leader = *(pthread_t *)argument;
    void *value = NULL;
    int result = pthread_join(leader, &value);
    if (result || value != (void *)(uintptr_t)0x51 || !prepared_identity(TEST_UID + 1, TEST_UID + 2)) {
        errno = result;
        _exit(die("leader_exit_preserves_thread_identity"));
    }
    /* Wait for the kernel's exit notification before entering waitpid.
     * Unlike a sleep or a user-space ready flag, this proves the child has
     * exited while its surviving parent thread has not started a wait. */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = child_notification;
    sigemptyset(&action.sa_mask);
    sigset_t children;
    sigemptyset(&children);
    sigaddset(&children, SIGCHLD);
    result = pthread_sigmask(SIG_BLOCK, &children, NULL);
    if (result || sigaction(SIGCHLD, &action, NULL)) {
        if (result) errno = result;
        _exit(die("leader_exit_worker_child_signal_setup"));
    }
    pid_t child = fork();
    if (!child) _exit(prepared_identity(TEST_UID + 1, TEST_UID + 2) ? 0 :
                      die("leader_exit_worker_fork_inherits_identity"));
    if (child < 0) _exit(die("leader_exit_worker_fork"));
    siginfo_t notification;
    struct timespec timeout = {5, 0};
    memset(&notification, 0, sizeof(notification));
    if (sigtimedwait(&children, &notification, &timeout) != SIGCHLD ||
        notification.si_pid != child || notification.si_code != CLD_EXITED ||
        notification.si_status != 0)
        _exit(die("leader_exit_worker_child_exited_before_wait"));
    if (wait_ok(child, "leader_exit_worker_child")) _exit(97);
    pthread_t peer;
    __atomic_store_n(&exit_peer_ready, 0, __ATOMIC_RELAXED);
    result = pthread_create(&peer, NULL, exit_group_peer, NULL);
    if (result) {
        errno = result;
        _exit(die("leader_exit_group_peer_create"));
    }
    while (!__atomic_load_n(&exit_peer_ready, __ATOMIC_ACQUIRE)) (void)sched_yield();
    /* The leader's earlier pthread_exit must not hide a later worker's
     * exit_group status, including a failing assertion in this test. The
     * group also has a live peer whose remote cancellation may finish later. */
    if (leader_exit_signal_mode) {
        (void)raise(SIGTERM);
        _exit(die("leader_exit_worker_fatal_signal_returned"));
    }
    _exit(37);
}

static int leader_exit_child(void)
{
    if (prepare_exec(TEST_UID + 1, TEST_UID + 2)) return 97;
    /* Storage stays valid after pthread_exit: it is copied into child memory,
     * rather than a pointer into the leader's potentially reclaimed stack. */
    static pthread_t leader;
    leader = pthread_self();
    pthread_t worker;
    int result = pthread_create(&worker, NULL, after_leader_exit, &leader);
    if (result) { errno = result; return die("leader_exit_create_worker"); }
    pthread_exit((void *)(uintptr_t)0x51);
}

static int leader_exit_case(int signal_exit)
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        if (arm_deadline()) _exit(97);
        leader_exit_signal_mode = signal_exit;
        _exit(leader_exit_child());
    }
    if (child < 0) return die("leader_exit_case_fork");
    int status;
    pid_t result;
    do result = waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
    if (result == child && (signal_exit ? WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM :
                            WIFEXITED(status) && WEXITSTATUS(status) == 37)) return 0;
    if (result == child) fprintf(stderr, "credentials leader-exit status=%d\n", status);
    return die(signal_exit ? "leader_exit_worker_fatal_signal_status" : "leader_exit_worker_exit_group_status");
}

struct orphan_control {
    uint32_t ready, release, done, result, exit_gate;
    pid_t pid, pgid;
};
struct orphan_worker_args {
    pthread_t leader;
    struct orphan_control *control;
    uint64_t deadline;
};
static uint64_t orphan_monotonic(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static int orphan_wait_flag(uint32_t *flag, uint64_t end)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (orphan_monotonic() >= end) return -1;
        (void)sched_yield();
    }
    return 0;
}
static int orphan_wait_child(pid_t child, uint64_t end)
{
    for (;;) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
        if ((result < 0 && errno != EINTR) || orphan_monotonic() >= end) return -1;
        (void)sched_yield();
    }
}
static void *orphan_worker(void *argument)
{
    struct orphan_worker_args *args = argument;
    struct orphan_control *control = args->control;
    void *value = NULL;
    int result = pthread_join(args->leader, &value);
    if (result || value != (void *)(uintptr_t)0x61) {
        if (result) errno = result;
        result = die("orphan_worker_join_leader");
    }
    /* B exits only after C's leader is dead and its worker has joined it. */
    __atomic_store_n(&control->ready, 1, __ATOMIC_RELEASE);
    if (!result && orphan_wait_flag(&control->release, args->deadline))
        result = die("orphan_worker_release_deadline");
    if (!result && (getpid() != __atomic_load_n(&control->pid, __ATOMIC_ACQUIRE) ||
                    getpgid(getpid()) != control->pgid ||
                    !prepared_identity(TEST_UID + 1, TEST_UID + 2)))
        result = die("orphan_worker_identity_after_parent_exit");
    if (!result) {
        pid_t child = fork();
        if (!child) _exit(prepared_identity(TEST_UID + 1, TEST_UID + 2) ? 0 :
                          die("orphan_worker_child_identity"));
        if (child < 0 || orphan_wait_child(child, args->deadline))
            result = die("orphan_worker_fork_and_wait");
    }
    __atomic_store_n(&control->result, (uint32_t)result, __ATOMIC_RELAXED);
    __atomic_store_n(&control->done, 1, __ATOMIC_RELEASE);
    /* This group has no parent left to reap it. A clock bound prevents a
     * failed observer from leaving the orphan worker alive indefinitely. */
    if (orphan_wait_flag(&control->exit_gate, args->deadline) && !result)
        result = die("orphan_worker_exit_gate_deadline");
    _exit(result);
}
static int orphan_process(struct orphan_control *control)
{
    static struct orphan_worker_args args;
    args.leader = pthread_self(); args.control = control;
    uint64_t now = orphan_monotonic();
    if (now == UINT64_MAX) return die("orphan_worker_clock");
    args.deadline = now + UINT64_C(6000000000);
    pthread_t worker;
    int result = pthread_create(&worker, NULL, orphan_worker, &args);
    if (result) { errno = result; return die("orphan_create_worker"); }
    pthread_exit((void *)(uintptr_t)0x61);
}
static int orphan_wait_reaped(struct orphan_control *control, uint64_t end)
{
    pid_t pid = __atomic_load_n(&control->pid, __ATOMIC_ACQUIRE);
    if (pid <= 0) return 0;
    for (;;) {
        errno = 0;
        if (getpgid(pid) == -1) return errno == ESRCH ? 0 : -1;
        if (orphan_monotonic() >= end) return -1;
        (void)sched_yield();
    }
}
static int orphan_live_thread_child(void)
{
    if (prepare_exec(TEST_UID + 1, TEST_UID + 2)) return 97;
    struct orphan_control *control = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (control == MAP_FAILED) return die("orphan_control_mapping");
    memset(control, 0, sizeof(*control));
    control->pgid = getpgid(0);
    uint64_t now = orphan_monotonic();
    if (control->pgid < 0 || now == UINT64_MAX) {
        (void)munmap(control, 4096);
        return die("orphan_observer_identity_clock");
    }
    uint64_t end = now + UINT64_C(8000000000);
    int failure = 0;
    pid_t intermediary = fork();
    if (!intermediary) {
        pid_t child = fork();
        if (!child) _exit(orphan_process(control));
        if (child < 0) _exit(die("orphan_intermediary_fork"));
        __atomic_store_n(&control->pid, child, __ATOMIC_RELEASE);
        if (orphan_wait_flag(&control->ready, now + UINT64_C(4000000000)))
            _exit(die("orphan_intermediary_ready_deadline"));
        _exit(0);
    }
    /* Reaping B proves that its orphan sweep ran before C may continue. */
    if (intermediary < 0 || orphan_wait_child(intermediary, end)) {
        failure = die("orphan_intermediary_status");
        goto release;
    }
    pid_t orphan = __atomic_load_n(&control->pid, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&control->ready, __ATOMIC_ACQUIRE) || orphan <= 0 ||
        getpgid(orphan) != control->pgid) {
        failure = die("orphan_dead_leader_retained_while_worker_live");
        goto release;
    }
    __atomic_store_n(&control->release, 1, __ATOMIC_RELEASE);
    if (orphan_wait_flag(&control->done, end) ||
        __atomic_load_n(&control->result, __ATOMIC_RELAXED) ||
        getpgid(orphan) != control->pgid)
        failure = die("orphan_live_thread_identity_and_child_wait");
release:
    __atomic_store_n(&control->release, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&control->exit_gate, 1, __ATOMIC_RELEASE);
    if (orphan_wait_reaped(control, end) && !failure)
        failure = die("orphan_whole_group_reap_deadline");
    if (munmap(control, 4096) && !failure) failure = die("orphan_control_unmap");
    return failure;
}

#define SETXID_STAGES 6
static pthread_barrier_t xid_barrier;
static struct identity xid_expected;
static int xid_failures[2];

static int barrier(void)
{
    int result = pthread_barrier_wait(&xid_barrier);
    return !result || result == PTHREAD_BARRIER_SERIAL_THREAD ? 0 : result;
}

static void *xid_observer(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    for (unsigned stage = 0; stage < SETXID_STAGES; stage++) {
        XID_SET(xid_worker_phase[index], stage * 10 + 1);
        if (barrier()) _exit(die("setxid_worker_start_barrier"));
        XID_SET(xid_worker_phase[index], stage * 10 + 2);
        struct identity observed;
        if (snapshot(&observed) || !same_identity(&observed, &xid_expected)) {
            xid_failures[index] = (int)stage + 1;
            fprintf(stderr, "credentials setxid worker=%u stage=%u identity mismatch\n", index, stage);
        }
        XID_SET(xid_worker_phase[index], stage * 10 + 3);
        if (barrier()) _exit(die("setxid_worker_done_barrier"));
        XID_SET(xid_worker_phase[index], stage * 10 + 4);
    }
    return NULL;
}

#define XID_STEP(phase, expression) (XID_SET(xid_phase, phase), (expression))
static int setxid_child(void)
{
    pthread_t workers[2];
    XID_SET(xid_stage, 0);
    XID_SET(xid_phase, 0);
    int result = pthread_barrier_init(&xid_barrier, NULL, 3);
    if (result) { errno = result; return die("setxid_barrier_init"); }
    for (unsigned i = 0; i < 2; i++) {
        XID_SET(xid_phase, 1 + i);
        result = pthread_create(&workers[i], NULL, xid_observer, (void *)(uintptr_t)i);
        if (result) { errno = result; return die("setxid_observer_create"); }
    }
    gid_t groups[] = {TEST_GID + 10, TEST_GID + 11};
    XID_SET(xid_phase, 4);
    if (snapshot(&xid_expected)) return die("setxid_initial_expected");
    for (unsigned stage = 0; stage < SETXID_STAGES; stage++) {
        XID_SET(xid_stage, stage);
        int failed = 0;
        if (!stage) {
            failed = XID_STEP(10, setgid(TEST_GID)) || XID_STEP(11, setgroups(2, groups)) ||
                XID_STEP(12, setgroups(2, groups));
            xid_expected.gid[0] = xid_expected.gid[1] = xid_expected.gid[2] = TEST_GID;
            xid_expected.count = 2;
            memcpy(xid_expected.groups, groups, sizeof(groups));
        } else if (stage == 1) {
            failed = XID_STEP(10, setresuid(TEST_UID, 0, 0));
            xid_expected.uid[0] = TEST_UID;
        } else if (stage == 2) {
            failed = XID_STEP(10, seteuid(TEST_UID)) || XID_STEP(11, seteuid(TEST_UID));
            xid_expected.uid[1] = TEST_UID;
        } else if (stage == 3) {
            failed = XID_STEP(10, seteuid(0)) || XID_STEP(11, setresgid(TEST_GID, TEST_GID + 1, TEST_GID + 2)) ||
                XID_STEP(12, setegid(TEST_GID + 1)) || XID_STEP(13, setegid(TEST_GID + 1));
            xid_expected.uid[1] = 0;
            xid_expected.gid[0] = TEST_GID;
            xid_expected.gid[1] = TEST_GID + 1;
            xid_expected.gid[2] = TEST_GID + 2;
        } else if (stage == 4) {
            failed = XID_STEP(10, setgid(TEST_GID + 3)) || XID_STEP(11, setgroups(2, groups)) ||
                XID_STEP(12, setuid(TEST_UID + 3)) || XID_STEP(13, setuid(TEST_UID + 3)) ||
                XID_STEP(14, setgid(TEST_GID + 3));
            xid_expected.uid[0] = xid_expected.uid[1] = xid_expected.uid[2] = TEST_UID + 3;
            xid_expected.gid[0] = xid_expected.gid[1] = xid_expected.gid[2] = TEST_GID + 3;
        } else {
            errno = 0;
            if (XID_STEP(10, setuid(0)) != -1 || errno != EPERM) failed = 1;
            errno = 0;
            if (XID_STEP(11, setgid(0)) != -1 || errno != EPERM) failed = 1;
            errno = 0;
            if (XID_STEP(12, setgroups(2, groups)) != -1 || errno != EPERM) failed = 1;
            if (XID_STEP(13, setresuid((uid_t)-1, (uid_t)-1, (uid_t)-1)) ||
                XID_STEP(14, setresgid((gid_t)-1, (gid_t)-1, (gid_t)-1))) failed = 1;
        }
        if (failed) {
            fprintf(stderr, "credentials setxid stage=%u failed\n", stage);
            return die("setxid_libc_transition");
        }
        struct identity actual;
        XID_SET(xid_phase, 20);
        if (snapshot(&actual) || !same_identity(&actual, &xid_expected))
            return die("setxid_main_expected_values");
        if (XID_STEP(21, barrier()) || XID_STEP(22, barrier())) return die("setxid_main_barriers");
        XID_SET(xid_phase, 23);
    }
    for (unsigned i = 0; i < 2; i++) {
        void *value = (void *)(uintptr_t)1;
        XID_SET(xid_phase, 30 + i);
        result = pthread_join(workers[i], &value);
        if (result || value || xid_failures[i]) {
            errno = result;
            return die("setxid_observers_join_and_identity");
        }
    }
    XID_SET(xid_phase, 32);
    result = pthread_barrier_destroy(&xid_barrier);
    if (result) { errno = result; return die("setxid_barrier_destroy"); }
    return 0;
}
#undef XID_STEP

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exec-identity")) return exec_stage(argc, argv);
    if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "qemu"))) return die("arguments");
    if (initial_auxv()) return 97;
    struct identity original, after;
    if (snapshot(&original)) return die("original_parent_identity");
    if (isolated(id_width_child, "id_width_status") || isolated(largest_id_child, "largest_id_status") ||
        isolated(sign_bit_id_child, "sign_bit_id_status")) return 97;
    puts("PASS glibc_credentials_id_width");
    if (isolated(saved_uid_child, "saved_uid_status") || isolated(saved_gid_child, "saved_gid_status") ||
        isolated(irreversible_child, "permanent_drop_status")) return 97;
    puts("PASS glibc_credentials_saved_uid_gid");
    if (isolated(groups_child, "groups_status") || isolated(empty_groups_child, "empty_groups_status")) return 97;
    puts("PASS glibc_credentials_groups_limit_atomic");
    if (isolated(fork_child, "fork_independent_status")) return 97;
    puts("PASS glibc_credentials_fork_independent");
    if (isolated(fault_child, "fault_rollback_status")) return 97;
    puts("PASS glibc_credentials_fault_rollback");
    if (isolated(unsupported_child, "unsupported_status")) return 97;
    puts("PASS glibc_credentials_unsupported_fsids");
#if defined(__arm__)
    puts("PASS glibc_credentials_arm_old16_explicit");
#endif
    if (exec_case(argv[0], 0, TEST_UID + 1) || exec_case(argv[0], 0, 0)) return 97;
    puts("PASS glibc_credentials_exec_identity");
    if (exec_case(argv[0], 1, TEST_UID + 1) || exec_case(argv[0], 1, 0)) return 97;
    puts("PASS glibc_credentials_nonleader_exec");
    if (leader_exit_case(0)) return 97;
    puts("PASS glibc_credentials_leader_exit");
    puts("PASS glibc_credentials_dead_leader_zombie_and_group_status");
    if (leader_exit_case(1)) return 97;
    puts("PASS glibc_credentials_dead_leader_signal_status");
    if (isolated(orphan_live_thread_child, "orphan_live_thread_status")) return 97;
    puts("PASS glibc_credentials_orphan_live_thread_identity");
    if (isolated(setxid_child, "nptl_setxid_status")) return 97;
    puts("PASS glibc_credentials_nptl_setxid");
    if (snapshot(&after) || !same_identity(&original, &after)) return die("root_orchestrator_identity_preserved");
    puts("PASS glibc_posix_credentials");
    return 0;
}
